#include "crowdy/session/world_session.hpp"

#include "crowdy/client.hpp"
#include "crowdy/core/base64.hpp"

namespace crowdy::session {

WorldSession::WorldSession(std::shared_ptr<replication::Connection> conn, CrowdyClient* client,
                           WorldSessionConfig config)
    : conn_(std::move(conn)), client_(client), config_(std::move(config)) {
  if (config_.actorUuid.size() == uuid_.size()) {
    core::actorUuidFromString(config_.actorUuid, uuid_);
  } else {
    uuid_ = core::generateActorUuid();
  }

  self_ = std::make_unique<LocalActorStore>(*conn_, uuid_, config_.self);
  actors_ = std::make_unique<RemoteActorStore>(uuid_, config_.actors);
  chunks_ = std::make_unique<ChunkStore>(*conn_, client_ ? &client_->chunks() : nullptr,
                                         config_.appId, config_.chunks);
  installHandlers();
}

WorldSession::~WorldSession() = default;

void WorldSession::installHandlers() {
  replication::Handlers handlers;
  handlers.actorUpdate = [this](const replication::SpatialNotification& n) {
    actors_->ingest(n, core::systemClock().monotonicMillis());
  };
  handlers.voxelUpdate = [this](const replication::SpatialNotification& n,
                                const wire::VoxelPayloadView& voxel) {
    chunks_->ingest(n, voxel);
  };
  handlers.genericError = [this](const replication::GenericError& e) {
    errors_.ingest(e, core::systemClock().monotonicMillis());
  };
  handlers.channelMessage = [this](const replication::ChannelNotification& n) {
    InboxMessage m;
    m.channelId = n.channelId;
    std::memcpy(m.senderUuid.data(), n.senderUuid, m.senderUuid.size());
    m.payload.assign(n.payload.begin(), n.payload.end());
    m.serverEpochMs = n.epochMillis;
    m.receivedAtMs = core::systemClock().monotonicMillis();
    channelInbox_.push(std::move(m));
  };
  handlers.singleActorMessage = [this](const replication::SpatialNotification& n) {
    InboxMessage m;
    m.channelId = 0;
    m.senderUuid = n.uuidArray();  // the TARGET uuid (you); sender is app-defined in payload
    m.payload.assign(n.payload.begin(), n.payload.end());
    m.serverEpochMs = n.epochMillis;
    m.receivedAtMs = core::systemClock().monotonicMillis();
    directInbox_.push(std::move(m));
  };
  conn_->setHandlers(std::move(handlers));
}

void WorldSession::tick() {
  const std::int64_t nowMs = core::systemClock().monotonicMillis();

  // 1) Drain inbound notifications into the stores.
  conn_->poll();

  // 2) Presence send loop.
  self_->tick(nowMs);
  if (auto seq = self_->lastSequence()) errors_.recordSend(*seq, SendKind::ActorUpdate);

  // 3) Staleness reaping.
  if (nowMs - lastReapMs_ >= config_.reapIntervalMs) {
    actors_->reap(nowMs);
    lastReapMs_ = nowMs;
  }

  // 4) Durable chunk write-back (throttled).
  chunks_->tick(nowMs);

  // 5) Host heartbeat (blocking GraphQL; opt out via interval 0).
  if (client_ && config_.hostHeartbeatIntervalMs > 0 &&
      nowMs - lastHostBeatMs_ >= config_.hostHeartbeatIntervalMs) {
    lastHostBeatMs_ = nowMs;
    try {
      client_->host().heartbeat(config_.appId);
      amIHost_ = client_->host().amIHost(config_.appId);
    } catch (const std::exception&) {
      // Host tracking is best-effort; next interval retries.
    }
  }
}

void WorldSession::dispose() {
  if (conn_) conn_->disconnect();
}

// ---------------------------------------------------------------------------
// ChunkStore GraphQL paths (here to keep the header free of domain includes)
// ---------------------------------------------------------------------------

std::size_t ChunkStore::ensureAround(const ChunkCoord& center, int distance) {
  if (!chunksApi_) return 0;
  graphql::Json page = chunksApi_->byDistance(
      appId_, domains::ChunkRef{center.x, center.y, center.z}, distance);
  graphql::Json list = page["chunks"];
  std::size_t hydrated = 0;
  const std::int64_t nowMs = core::systemClock().monotonicMillis();
  list.forEach([&](graphql::Json chunkJson) {
    ChunkCoord coord{chunkJson["coordinates"]["x"].asBigInt(),
                     chunkJson["coordinates"]["y"].asBigInt(),
                     chunkJson["coordinates"]["z"].asBigInt()};
    ChunkData& c = chunks_[coord];
    c.coord = coord;
    c.storedOnServer = true;
    c.hydratedAtMs = nowMs;
    auto voxels = core::base64Decode(chunkJson["voxels"].asStringView());
    if (voxels && voxels->size() == c.voxels.size()) {
      std::memcpy(c.voxels.data(), voxels->data(), c.voxels.size());
    }
    ++hydrated;
  });
  return hydrated;
}

void ChunkStore::tick(std::int64_t nowMs) {
  if (!chunksApi_ || options_.writeBackIntervalMs <= 0) return;
  if (nowMs - lastWriteBackMs_ < options_.writeBackIntervalMs) return;

  for (auto& [coord, chunk] : chunks_) {
    if (!chunk.dirty) continue;
    lastWriteBackMs_ = nowMs;
    try {
      graphql::JVal input;
      input["appId"] = appId_;
      input["chunk"] = domains::ChunkRef{coord.x, coord.y, coord.z}.toInput();
      input["voxels"] = core::base64Encode(Bytes(chunk.voxels.data(), chunk.voxels.size()));
      chunksApi_->update(input);
      chunk.dirty = false;
      chunk.storedOnServer = true;
    } catch (const std::exception&) {
      // Leave dirty; retried on a later tick.
    }
    break;  // at most one chunk per interval
  }
}

}  // namespace crowdy::session
