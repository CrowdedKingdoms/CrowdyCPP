#pragma once

#include <memory>
#include <string>

#include "crowdy/session/chunk_store.hpp"
#include "crowdy/session/stores.hpp"

namespace crowdy {
class CrowdyClient;
}

/// WorldSession — SDK-managed game state over ONE replication connection and
/// one tick, mirroring CrowdyJS's createWorldSession:
///
///   - self():    your actor (uuid minted, presence at a fixed Hz with
///                send-on-change + keyframes + idle heartbeats)
///   - actors():  remote-actor registry (self-filtered, staleness-reaped,
///                sample history for interpolation)
///   - chunks():  chunk/voxel cache (bulk hydrate, realtime merge, optimistic
///                edits, worldgen write-back)
///   - errors():  server-reported send errors attributed to send kinds
///   - channelInbox() / directInbox(): message queues
///   - hostTracker: periodic host heartbeat + amIHost cache
///
/// Single-threaded: call tick(nowMs) from your game loop; it polls the
/// connection (dispatching into the stores), drives the send loop, reaps
/// stale actors, writes back dirty chunks, and heartbeats host eligibility.
/// All reads are plain snapshots on the same thread.
namespace crowdy::session {

struct WorldSessionConfig {
  std::string appId;
  LocalActorStore::Options self;
  RemoteActorStore::Options actors;
  ChunkStore::Options chunks;
  /// Send host.heartbeat over GraphQL this often (0 disables host tracking).
  /// Note: this is a blocking HTTP call on the tick thread; disable it and
  /// run your own if that is unacceptable.
  std::int64_t hostHeartbeatIntervalMs = 3000;
  /// Reap stale remote actors this often.
  std::int64_t reapIntervalMs = 1000;
  /// Persist this actor uuid instead of minting a fresh one.
  std::string actorUuid;
};

class WorldSession {
 public:
  /// Build over an already-connected replication connection. `client` may be
  /// null for offline/tests (chunk hydrate + host tracking are then off).
  WorldSession(std::shared_ptr<replication::Connection> conn, CrowdyClient* client,
               WorldSessionConfig config);
  ~WorldSession();

  LocalActorStore& self() { return *self_; }
  RemoteActorStore& actors() { return *actors_; }
  ChunkStore& chunks() { return *chunks_; }
  ErrorStore& errors() { return errors_; }
  Inbox& channelInbox() { return channelInbox_; }
  Inbox& directInbox() { return directInbox_; }
  replication::Connection& connection() { return *conn_; }

  const core::ActorUuid& actorUuid() const { return uuid_; }
  bool amIHost() const { return amIHost_; }

  /// Join the world (first actor update) at `chunk` with the initial state.
  Status join(const ChunkCoord& chunk, Bytes initialState) {
    return self_->join(chunk, initialState);
  }

  /// Drive everything. Call once per frame from the game loop.
  void tick();

  /// Dispose: disconnects the replication connection.
  void dispose();

 private:
  void installHandlers();

  std::shared_ptr<replication::Connection> conn_;
  CrowdyClient* client_;
  WorldSessionConfig config_;
  core::ActorUuid uuid_{};

  std::unique_ptr<LocalActorStore> self_;
  std::unique_ptr<RemoteActorStore> actors_;
  std::unique_ptr<ChunkStore> chunks_;
  ErrorStore errors_;
  Inbox channelInbox_;
  Inbox directInbox_;

  bool amIHost_ = false;
  std::int64_t lastReapMs_ = 0;
  std::int64_t lastHostBeatMs_ = 0;
};

}  // namespace crowdy::session
