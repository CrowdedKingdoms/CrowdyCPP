#include "crowdy/core/uuid.hpp"

#include <cstring>

namespace crowdy::core {

ActorUuid generateActorUuid(const ICrypto& crypto) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::uint8_t raw[16];
  crypto.randomBytes(raw, sizeof(raw));
  ActorUuid out{};
  for (std::size_t i = 0; i < 16; ++i) {
    out[i * 2] = kHex[raw[i] >> 4];
    out[i * 2 + 1] = kHex[raw[i] & 0x0f];
  }
  return out;
}

bool actorUuidFromString(std::string_view s, ActorUuid& out) {
  if (s.size() != out.size()) return false;
  std::memcpy(out.data(), s.data(), out.size());
  return true;
}

}  // namespace crowdy::core
