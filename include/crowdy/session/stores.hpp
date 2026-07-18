#pragma once

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/keys.hpp"

/// Session stores — the C++ analog of CrowdyJS's World Stores: the
/// source-of-truth data structures every game otherwise hand-writes, driven
/// by ONE replication connection and one tick.
///
/// Threading: all stores are single-threaded by design. WorldSession::tick()
/// polls the connection (dispatching handlers into the stores) on the calling
/// thread — the same thread that reads them. Reads are plain snapshots.
namespace crowdy::session {

/// Maximum actor-state payload the stores handle (well under the ~1.1 KB
/// spatial payload budget; keep real poses far smaller).
constexpr std::size_t kMaxStateBytes = 256;

struct StateBlob {
  std::uint8_t data[kMaxStateBytes];
  std::uint16_t len = 0;

  Bytes bytes() const { return Bytes(data, len); }
  void assign(Bytes b) {
    len = static_cast<std::uint16_t>(b.size() > kMaxStateBytes ? kMaxStateBytes : b.size());
    if (len > 0) std::memcpy(data, b.data(), len);
  }
  bool equals(Bytes b) const {
    return b.size() == len && (len == 0 || std::memcmp(data, b.data(), len) == 0);
  }
};

// ---------------------------------------------------------------------------
// LocalActorStore — your own actor: presence loop with send-on-change
// ---------------------------------------------------------------------------

/// Drives the local actor's presence: joins by sending the first actor
/// update, then re-sends at a fixed rate with send-on-change dedup and
/// periodic keyframes, and falls back to cheap heartbeats while idle.
class LocalActorStore {
 public:
  struct Options {
    int sendHz = 5;
    /// Force a full send at least this often even when unchanged (keeps
    /// presence fresh and repairs lost packets).
    std::int64_t keyframeIntervalMs = 3000;
    /// While unchanged, send CLIENT_ACTOR_HEARTBEAT at this interval instead
    /// of a full update (0 disables heartbeats; keyframes still flow).
    std::int64_t heartbeatIntervalMs = 2000;
    std::uint8_t distance = 8;
    wire::DecayRate decay = wire::DecayRate::Exponential;
  };

  LocalActorStore(replication::Connection& conn, core::ActorUuid uuid, Options options)
      : conn_(conn), uuid_(uuid), options_(options) {}

  const core::ActorUuid& uuid() const { return uuid_; }
  const ChunkCoord& chunk() const { return chunk_; }
  bool joined() const { return joined_; }

  /// Join the world: set the starting chunk and send the first actor update.
  Status join(const ChunkCoord& chunk, Bytes initialState) {
    chunk_ = chunk;
    state_.assign(initialState);
    dirty_ = true;
    joined_ = true;
    return sendNow(/*nowMs=*/0);
  }

  /// Update the actor's chunk (as it moves across chunk boundaries).
  void setChunk(const ChunkCoord& chunk) {
    if (!(chunk == chunk_)) {
      chunk_ = chunk;
      dirty_ = true;
    }
  }

  /// Replace the replicated state; a change is sent on the next tick slot.
  void setState(Bytes state) {
    if (!state_.equals(state)) {
      state_.assign(state);
      dirty_ = true;
    }
  }

  /// Drive the send loop. Call every frame with a monotonic clock.
  void tick(std::int64_t nowMs) {
    if (!joined_) return;
    const std::int64_t interval = 1000 / (options_.sendHz > 0 ? options_.sendHz : 5);
    if (nowMs - lastSendMs_ < interval) return;

    const bool keyframeDue = nowMs - lastFullSendMs_ >= options_.keyframeIntervalMs;
    if (dirty_ || keyframeDue) {
      sendNow(nowMs);
    } else if (options_.heartbeatIntervalMs > 0 &&
               nowMs - lastHeartbeatMs_ >= options_.heartbeatIntervalMs) {
      conn_.sendHeartbeat(chunk_, uuid_);
      lastHeartbeatMs_ = nowMs;
      lastSendMs_ = nowMs;
    }
  }

  std::optional<std::uint8_t> lastSequence() const { return lastSequence_; }

 private:
  Status sendNow(std::int64_t nowMs) {
    replication::SpatialSend p;
    p.chunk = chunk_;
    p.uuid = uuid_;
    p.payload = state_.bytes();
    p.distance = options_.distance;
    p.decay = options_.decay;
    auto seq = conn_.sendActorUpdate(p);
    if (seq.ok()) {
      lastSequence_ = seq.value();
      dirty_ = false;
      lastSendMs_ = nowMs;
      lastFullSendMs_ = nowMs;
    }
    return seq.ok() ? Status(Errc::Ok) : Status(seq.error());
  }

  replication::Connection& conn_;
  core::ActorUuid uuid_;
  Options options_;
  ChunkCoord chunk_{};
  StateBlob state_{};
  bool joined_ = false;
  bool dirty_ = false;
  std::int64_t lastSendMs_ = 0;
  std::int64_t lastFullSendMs_ = 0;
  std::int64_t lastHeartbeatMs_ = 0;
  std::optional<std::uint8_t> lastSequence_;
};

// ---------------------------------------------------------------------------
// RemoteActorStore — everyone else: registry with staleness + sample history
// ---------------------------------------------------------------------------

struct ActorSample {
  StateBlob state;
  std::int64_t serverEpochMs = 0;
  std::int64_t receivedAtMs = 0;
};

struct RemoteActor {
  core::ActorUuid uuid{};
  ChunkCoord chunk{};
  std::int64_t lastSeenMs = 0;         ///< monotonic, for staleness
  std::int64_t lastServerEpochMs = 0;  ///< server time of the latest update
  /// Newest-first ring of recent samples (for interpolation).
  std::vector<ActorSample> samples;

  Bytes state() const { return samples.empty() ? Bytes() : samples.front().state.bytes(); }
};

class RemoteActorStore {
 public:
  struct Options {
    std::int64_t staleAfterMs = 12000;
    int historySize = 2;
  };

  RemoteActorStore(core::ActorUuid selfUuid, Options options)
      : selfUuid_(selfUuid), options_(options) {}

  /// Ingest an ACTOR_UPDATE_NOTIFICATION (self-filtered).
  void ingest(const replication::SpatialNotification& n, std::int64_t nowMs) {
    const core::ActorUuid uuid = n.uuidArray();
    if (uuid == selfUuid_) return;
    RemoteActor& actor = actors_[uuid];
    actor.uuid = uuid;
    actor.chunk = n.chunk;
    actor.lastSeenMs = nowMs;
    actor.lastServerEpochMs = n.epochMillis;

    ActorSample sample;
    sample.state.assign(n.payload);
    sample.serverEpochMs = n.epochMillis;
    sample.receivedAtMs = nowMs;
    actor.samples.insert(actor.samples.begin(), std::move(sample));
    if (actor.samples.size() > static_cast<std::size_t>(options_.historySize))
      actor.samples.resize(static_cast<std::size_t>(options_.historySize));
  }

  /// Drop actors not seen within staleAfterMs.
  void reap(std::int64_t nowMs) {
    for (auto it = actors_.begin(); it != actors_.end();) {
      if (nowMs - it->second.lastSeenMs > options_.staleAfterMs) {
        it = actors_.erase(it);
      } else {
        ++it;
      }
    }
  }

  const RemoteActor* find(const core::ActorUuid& uuid) const {
    auto it = actors_.find(uuid);
    return it == actors_.end() ? nullptr : &it->second;
  }

  std::size_t size() const { return actors_.size(); }

  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (const auto& [uuid, actor] : actors_) fn(actor);
  }

 private:
  core::ActorUuid selfUuid_;
  Options options_;
  std::unordered_map<core::ActorUuid, RemoteActor, ActorUuidHash> actors_;
};

// ---------------------------------------------------------------------------
// ErrorStore — server-reported send errors attributed to what you sent
// ---------------------------------------------------------------------------

enum class SendKind : std::uint8_t {
  Unknown = 0,
  ActorUpdate,
  VoxelUpdate,
  Audio,
  Text,
  ClientEvent,
  GenericSpatial,
  SingleActor,
  Channel,
  Heartbeat,
};

struct AttributedError {
  wire::ErrorCode code;
  std::uint8_t sequence;
  SendKind kind;
  std::int64_t atMs;
};

/// Correlates GENERIC_ERROR_MESSAGE frames (sequence-numbered, uint8 wrap)
/// with the kind of send that used that sequence.
class ErrorStore {
 public:
  void recordSend(std::uint8_t sequence, SendKind kind) { sent_[sequence] = kind; }

  void ingest(const replication::GenericError& e, std::int64_t nowMs) {
    AttributedError err{e.code, e.sequence, sent_[e.sequence], nowMs};
    recent_.push_back(err);
    if (recent_.size() > kMaxErrors) recent_.pop_front();
  }

  const std::deque<AttributedError>& recent() const { return recent_; }
  void clear() { recent_.clear(); }

 private:
  static constexpr std::size_t kMaxErrors = 64;
  SendKind sent_[256] = {};
  std::deque<AttributedError> recent_;
};

// ---------------------------------------------------------------------------
// Inboxes — channel + direct messages
// ---------------------------------------------------------------------------

struct InboxMessage {
  std::int64_t channelId = 0;  ///< 0 for direct (single-actor) messages
  core::ActorUuid senderUuid{};  ///< for direct messages: the TARGET uuid (you)
  std::vector<std::uint8_t> payload;
  std::int64_t serverEpochMs = 0;
  std::int64_t receivedAtMs = 0;
};

class Inbox {
 public:
  explicit Inbox(std::size_t maxMessages = 256) : max_(maxMessages) {}

  void push(InboxMessage message) {
    messages_.push_back(std::move(message));
    if (messages_.size() > max_) messages_.pop_front();
  }

  /// Drain up to `max` messages (oldest first).
  std::vector<InboxMessage> drain(std::size_t max = SIZE_MAX) {
    std::vector<InboxMessage> out;
    while (!messages_.empty() && out.size() < max) {
      out.push_back(std::move(messages_.front()));
      messages_.pop_front();
    }
    return out;
  }

  std::size_t size() const { return messages_.size(); }

 private:
  std::size_t max_;
  std::deque<InboxMessage> messages_;
};

}  // namespace crowdy::session
