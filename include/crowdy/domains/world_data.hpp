#pragma once

#include <string>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// World-data domains (Game API): chunks, voxels, actors, avatars, per-user
/// app state, host election, and teleport. Durable storage — realtime edits
/// travel over the replication client instead.
namespace crowdy::domains {

/// client.chunks() — durable chunk storage. `voxels` blobs are base64 4096-
/// byte dense grids (index x + y*16 + z*256).
class ChunksAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json get(std::string_view appId, const ChunkRef& chunk) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["chunk"] = chunk.toInput();
    return execUnwrap(gen::chunks::kGetChunkDocument, vars);
  }

  graphql::Json getLods(std::string_view appId, const ChunkRef& chunk) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["chunk"] = chunk.toInput();
    return execUnwrap(gen::chunks::kGetChunkLodsDocument, vars);
  }

  /// Bulk-load every stored chunk within `distance` of `chunk` (Chebyshev).
  graphql::Json byDistance(std::string_view appId, const ChunkRef& chunk, int distance) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["chunk"] = chunk.toInput();
    vars["input"]["distance"] = std::int64_t{distance};
    return execUnwrap(gen::chunks::kGetChunksByDistanceDocument, vars);
  }

  graphql::Json voxelList(std::string_view appId, const ChunkRef& chunk) const {
    graphql::JVal vars;
    vars["input"]["appId"] = appId;
    vars["input"]["chunk"] = chunk.toInput();
    return execUnwrap(gen::chunks::kGetVoxelListDocument, vars);
  }

  /// Persist a chunk (input: appId, chunk, voxels base64, optional state).
  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::chunks::kUpdateChunkDocument, vars);
  }

  graphql::Json updateState(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::chunks::kUpdateChunkStateDocument, vars);
  }

  graphql::Json updateLods(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::chunks::kUpdateChunkLodsDocument, vars);
  }
};

/// client.voxels() — voxel reads, durable updates, history + moderation.
class VoxelsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json list(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kListVoxelsDocument, vars);
  }

  graphql::Json listByDistance(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kListVoxelUpdatesByDistanceDocument, vars);
  }

  /// Durable single-voxel update (requires update_voxel_data permission).
  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kUpdateVoxelDocument, vars);
  }

  graphql::Json history(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kVoxelUpdateHistoryDocument, vars,
                      "VoxelUpdateHistory");
  }

  /// Relay cursor pagination variant of history().
  graphql::Json historyConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::voxels::kVoxelUpdateHistoryDocument, vars,
                      "VoxelUpdateHistoryConnection");
  }

  /// Moderation: revert a region/user's voxel edits. Accepts idempotencyKey
  /// inside the input.
  graphql::Json rollback(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::voxels::kRollbackVoxelUpdatesDocument, vars);
  }
};

/// client.actors() — durable actor records (realtime presence is UDP-side).
class ActorsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json get(std::string_view uuid) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    return execUnwrap(gen::actors::kActorDocument, vars);
  }

  graphql::Json list(const graphql::JVal& filter = graphql::JVal()) const {
    graphql::JVal vars;
    if (!filter.isNull()) vars["filter"] = filter;
    return execUnwrap(gen::actors::kActorsDocument, vars, "Actors");
  }

  /// Relay cursor pagination variant of list().
  graphql::Json listConnection(int first = 50, std::string_view after = {},
                               const graphql::JVal& filter = graphql::JVal()) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!filter.isNull()) vars["filter"] = filter;
    return execUnwrap(gen::actors::kActorsDocument, vars, "ActorsConnection");
  }

  graphql::Json batchLookup(const std::vector<std::string>& uuids) const {
    graphql::JArray arr;
    for (const auto& u : uuids) arr.emplace_back(u);
    graphql::JVal vars;
    vars["input"]["uuids"] = graphql::JVal(std::move(arr));
    return execUnwrap(gen::actors::kBatchLookupActorsDocument, vars);
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::actors::kCreateActorDocument, vars);
  }

  graphql::Json update(std::string_view uuid, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    vars["input"] = input;
    return execUnwrap(gen::actors::kUpdateActorDocument, vars);
  }

  /// Destructive; pass a stable idempotencyKey to make retries safe.
  graphql::Json remove(std::string_view uuid, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::actors::kDeleteActorDocument, vars);
  }

  graphql::Json updateState(std::string_view uuid, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["uuid"] = uuid;
    vars["input"] = input;
    return execUnwrap(gen::actors::kUpdateActorStateDocument, vars);
  }
};

/// client.avatars() — characters + per-app avatar state.
class AvatarsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json mine() const {
    return execUnwrap(gen::avatars::kAvatarsDocument, graphql::JVal(), "MyAvatars");
  }

  graphql::Json listForUser(std::string_view userId) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "UserAvatars");
  }

  graphql::Json get(std::string_view id) const {
    graphql::JVal vars;
    vars["id"] = id;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "AvatarById");
  }

  graphql::Json appState(std::string_view appId, std::string_view avatarId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["avatarId"] = avatarId;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "AvatarAppState");
  }

  graphql::Json appStates(std::string_view appId, const std::vector<std::string>& avatarIds) const {
    graphql::JArray arr;
    for (const auto& id : avatarIds) arr.emplace_back(id);
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["avatarIds"] = graphql::JVal(std::move(arr));
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "AvatarAppStates");
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "CreateAvatar");
  }

  graphql::Json update(std::string_view id, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["id"] = id;
    vars["input"] = input;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "UpdateAvatar");
  }

  graphql::Json remove(std::string_view id, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["id"] = id;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "DeleteAvatar");
  }

  graphql::Json updateState(std::string_view id, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["id"] = id;
    vars["input"] = input;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "UpdateAvatarState");
  }

  graphql::Json updateAppState(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::avatars::kAvatarsDocument, vars, "UpdateAvatarAppState");
  }
};

/// client.state() — per-user per-app save blob.
class StateAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json getOne(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::state::kUserAppStateDocument, vars);
  }

  graphql::Json getAll() const { return execUnwrap(gen::state::kUserAppStatesDocument); }

  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::state::kUpdateUserAppStateDocument, vars);
  }

  graphql::Json remove(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::state::kDeleteUserAppStateDocument, vars);
  }
};

/// client.host() — host election reads + actor liveness heartbeat. The
/// election is informational; authoritative gating uses the game model's
/// is_host invoke policy.
class HostAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json get(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::host::kHostDocument, vars, "GameHost");
  }

  bool amIHost(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::host::kHostDocument, vars, "AmIGameHost").asBool();
  }

  /// Send ~every 3 s while connected to stay host-eligible; returns the
  /// current host.
  graphql::Json heartbeat(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::host::kHostDocument, vars, "ActorHeartbeat");
  }
};

/// client.teleport() — teleport requests.
class TeleportAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json request(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teleport::kTeleportRequestDocument, vars);
  }
};

}  // namespace crowdy::domains
