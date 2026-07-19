#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/graphql/json.hpp"

/// Engine wire registry — the client mirror of the server's
/// `crowdy-game-kit-core::wire` (the single source of truth for the actor
/// pose layout and flag-bit registry used by compute-module game engines).
/// Parity with the Rust crate (and CrowdyJS's kit/wire) is checked by
/// kit_test.cpp: any change here must land in kit-core first.
///
/// 48-byte little-endian pose: pos f32 x3 (0..11), yaw/pitch f32 (12..19),
/// velocity f32 x3 (20..31), flags u8 (32), held u8 (33), 34-35 reserved,
/// updated_at f64 ms (36..43), 44-47 reserved. Payloads may append opaque
/// UTF-8 suffix bytes (engines put the entity's container id there).
namespace crowdy::kit {

inline constexpr std::size_t kPoseBytes = 48;

/// Flag-bit registry. Bits 0-3 are platform-reserved; games may use 4-7.
inline constexpr std::uint8_t kFlagGrounded = 0b0001;
inline constexpr std::uint8_t kFlagMob = 0b0010;
inline constexpr std::uint8_t kFlagNpc = 0b0100;
inline constexpr std::uint8_t kFlagReserved3 = 0b1000;

/// The decoded engine pose (plus the payload suffix, when present).
struct EnginePose {
  float x = 0, y = 0, z = 0;
  float yaw = 0, pitch = 0;
  float velX = 0, velY = 0, velZ = 0;
  std::uint8_t flags = 0;
  std::uint8_t held = 0;
  double updatedAtMs = 0;
  /// UTF-8 payload suffix after the 48-byte pose (container id), if any.
  std::string suffix;

  bool isMob() const { return (flags & kFlagMob) != 0; }
  bool isNpc() const { return (flags & kFlagNpc) != 0; }
  bool isPlayer() const { return (flags & (kFlagMob | kFlagNpc)) == 0; }
};

namespace detail {
inline void putF32(std::uint8_t* out, float v) { std::memcpy(out, &v, 4); }
inline void putF64(std::uint8_t* out, double v) { std::memcpy(out, &v, 8); }
inline float getF32(const std::uint8_t* in) {
  float v;
  std::memcpy(&v, in, 4);
  return v;
}
inline double getF64(const std::uint8_t* in) {
  double v;
  std::memcpy(&v, in, 8);
  return v;
}
inline bool finite3(float a, float b, float c) {
  return a == a && b == b && c == c && a - a == 0 && b - b == 0 && c - c == 0;
}
}  // namespace detail

/// Encode a pose (suffix appended when set). Little-endian layout — matches
/// the platform wire on every supported target.
inline std::vector<std::uint8_t> encodeEnginePose(const EnginePose& pose) {
  std::vector<std::uint8_t> bytes(kPoseBytes + pose.suffix.size(), 0);
  detail::putF32(&bytes[0], pose.x);
  detail::putF32(&bytes[4], pose.y);
  detail::putF32(&bytes[8], pose.z);
  detail::putF32(&bytes[12], pose.yaw);
  detail::putF32(&bytes[16], pose.pitch);
  detail::putF32(&bytes[20], pose.velX);
  detail::putF32(&bytes[24], pose.velY);
  detail::putF32(&bytes[28], pose.velZ);
  bytes[32] = pose.flags;
  bytes[33] = pose.held;
  detail::putF64(&bytes[36], pose.updatedAtMs);
  std::memcpy(bytes.data() + kPoseBytes, pose.suffix.data(), pose.suffix.size());
  return bytes;
}

/// The UTF-8 suffix after the pose (engine container ids), if any.
inline std::string poseSuffix(const std::uint8_t* bytes, std::size_t len) {
  if (len <= kPoseBytes) return {};
  std::string text(reinterpret_cast<const char*>(bytes) + kPoseBytes, len - kPoseBytes);
  // trim
  while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
  std::size_t start = 0;
  while (start < text.size() && text[start] == ' ') ++start;
  return text.substr(start);
}

/// Decode the leading 48 bytes (tolerates longer payloads — the suffix is
/// extracted). nullopt for short or non-finite payloads.
inline std::optional<EnginePose> decodeEnginePose(const std::uint8_t* bytes, std::size_t len) {
  if (len < kPoseBytes) return std::nullopt;
  EnginePose pose;
  pose.x = detail::getF32(&bytes[0]);
  pose.y = detail::getF32(&bytes[4]);
  pose.z = detail::getF32(&bytes[8]);
  pose.yaw = detail::getF32(&bytes[12]);
  pose.pitch = detail::getF32(&bytes[16]);
  pose.velX = detail::getF32(&bytes[20]);
  pose.velY = detail::getF32(&bytes[24]);
  pose.velZ = detail::getF32(&bytes[28]);
  pose.flags = bytes[32];
  pose.held = bytes[33];
  pose.updatedAtMs = detail::getF64(&bytes[36]);
  pose.suffix = poseSuffix(bytes, len);
  if (!detail::finite3(pose.x, pose.y, pose.z)) return std::nullopt;
  return pose;
}

inline std::optional<EnginePose> decodeEnginePose(const std::vector<std::uint8_t>& bytes) {
  return decodeEnginePose(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// Server-event payloads ([u16 LE event type][state bytes], state = JSON)
// ---------------------------------------------------------------------------

/// Contact damage decided by a mob/combat engine (kit-play referee).
inline constexpr std::uint16_t kEventContactDamage = 77;
/// Weather/season transition from a world engine (kit-sim weather).
inline constexpr std::uint16_t kEventWeather = 90;

/// A split engine server-event payload: type + parsed JSON body.
struct EngineEvent {
  std::uint16_t eventType = 0;
  graphql::Json body;
};

/// Split an engine server-event payload; nullopt when too short / not JSON.
inline std::optional<EngineEvent> parseEngineEvent(const std::uint8_t* bytes, std::size_t len) {
  if (len < 2) return std::nullopt;
  EngineEvent event;
  event.eventType = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
  if (len > 2) {
    event.body = graphql::Json::parse(
        std::string_view(reinterpret_cast<const char*>(bytes) + 2, len - 2));
    if (!event.body.ok()) return std::nullopt;
  }
  return event;
}

/// A parsed type-77 contact-damage event.
struct ContactDamageEvent {
  std::string targetUuid;
  std::int64_t damage = 0;
  std::string mobId;
  std::string mobName;
};

/// Parse a contact-damage event; nullopt when the payload is another type.
inline std::optional<ContactDamageEvent> parseContactDamage(const std::uint8_t* bytes,
                                                            std::size_t len) {
  auto event = parseEngineEvent(bytes, len);
  if (!event || event->eventType != kEventContactDamage) return std::nullopt;
  ContactDamageEvent out;
  out.targetUuid = event->body["targetUuid"].asString();
  out.damage = event->body["damage"].asInt64();
  out.mobId = event->body["mobId"].asString();
  out.mobName = event->body["mobName"].asString();
  return out;
}

/// A parsed type-90 weather transition event.
struct WeatherEvent {
  std::string weather;
  std::int64_t sinceMs = 0;
  std::int64_t untilMs = 0;
  /// The full body (dayPhase, isNight, remainingMs, ...).
  graphql::Json body;
};

/// Parse a weather event; nullopt when the payload is another type.
inline std::optional<WeatherEvent> parseWeatherEvent(const std::uint8_t* bytes, std::size_t len) {
  auto event = parseEngineEvent(bytes, len);
  if (!event || event->eventType != kEventWeather) return std::nullopt;
  WeatherEvent out;
  out.weather = event->body["weather"].asString();
  out.sinceMs = event->body["sinceMs"].asInt64();
  out.untilMs = event->body["untilMs"].asInt64();
  out.body = event->body;
  return out;
}

}  // namespace crowdy::kit
