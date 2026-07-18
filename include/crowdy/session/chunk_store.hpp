#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "crowdy/replication/connection.hpp"
#include "crowdy/session/keys.hpp"

namespace crowdy::domains {
class ChunksAPI;
}

/// ChunkStore — a chunk/voxel cache: bulk hydrate over GraphQL, realtime
/// merge from voxel notifications, optimistic local edits with UDP sends, and
/// optional durable write-back for locally generated chunks (the shared-
/// worldgen pattern).
namespace crowdy::session {

struct VoxelState {
  std::int16_t voxelType = 0;
  std::vector<std::uint8_t> state;
};

struct ChunkData {
  ChunkCoord coord{};
  /// Dense 16^3 voxel-type grid (index x + y*16 + z*256).
  std::array<std::uint8_t, kChunkVolume> voxels{};
  /// Sparse per-voxel metadata blobs, keyed by voxel index.
  std::unordered_map<int, VoxelState> voxelStates;
  bool storedOnServer = false;  ///< false = locally generated, pending write-back
  bool dirty = false;           ///< local edits not yet persisted
  std::int64_t hydratedAtMs = 0;
};

class ChunkStore {
 public:
  struct Options {
    /// Persist locally generated / edited chunks back through chunks.update,
    /// at most one chunk per writeBackIntervalMs (0 disables write-back).
    std::int64_t writeBackIntervalMs = 700;
    std::uint8_t voxelSendDistance = 8;
  };

  ChunkStore(replication::Connection& conn, domains::ChunksAPI* chunksApi, std::string appId,
             Options options)
      : conn_(conn), chunksApi_(chunksApi), appId_(std::move(appId)), options_(options) {}

  /// Load every stored chunk within `distance` of `center` from the durable
  /// store (one GraphQL round trip). Coordinates already cached are refreshed.
  /// Requires a ChunksAPI (throws GraphQL errors on failure).
  std::size_t ensureAround(const ChunkCoord& center, int distance);

  /// Look up a cached chunk (nullptr when absent).
  const ChunkData* find(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    return it == chunks_.end() ? nullptr : &it->second;
  }

  /// Insert a locally generated chunk (worldgen write-back pattern: chunks
  /// the server has never stored are generated client-side and persisted so
  /// the world stays identical for everyone).
  ChunkData& insertGenerated(const ChunkCoord& coord,
                             const std::array<std::uint8_t, kChunkVolume>& voxels) {
    ChunkData& c = chunks_[coord];
    c.coord = coord;
    c.voxels = voxels;
    c.storedOnServer = false;
    c.dirty = options_.writeBackIntervalMs > 0;
    return c;
  }

  /// Optimistic edit: apply locally, replicate over UDP, mark for durable
  /// write-back. Returns the send's sequence number.
  Result<std::uint8_t> setVoxel(const ChunkCoord& coord, int x, int y, int z,
                                std::int16_t voxelType, Bytes voxelState,
                                const core::ActorUuid& uuid) {
    if (x < 0 || x >= kChunkSize || y < 0 || y >= kChunkSize || z < 0 || z >= kChunkSize)
      return Errc::InvalidArgument;
    applyLocal(coord, x, y, z, voxelType, voxelState);
    auto seq = conn_.sendVoxelUpdate(coord, uuid, static_cast<std::int16_t>(x),
                                     static_cast<std::int16_t>(y), static_cast<std::int16_t>(z),
                                     voxelType, voxelState, options_.voxelSendDistance);
    return seq;
  }

  /// Merge an inbound VOXEL_UPDATE_NOTIFICATION.
  void ingest(const replication::SpatialNotification& n, const wire::VoxelPayloadView& voxel) {
    applyLocal(n.chunk, voxel.x, voxel.y, voxel.z, voxel.voxelType, voxel.state,
               /*markDirty=*/false);
  }

  /// Drive throttled durable write-back. Call from the session tick.
  void tick(std::int64_t nowMs);

  std::size_t size() const { return chunks_.size(); }

 private:
  void applyLocal(const ChunkCoord& coord, int x, int y, int z, std::int16_t voxelType,
                  Bytes state, bool markDirty = true) {
    ChunkData& c = chunks_[coord];
    c.coord = coord;
    const int index = voxelIndex(x, y, z);
    c.voxels[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(voxelType);
    if (state.empty()) {
      c.voxelStates.erase(index);
    } else {
      VoxelState& vs = c.voxelStates[index];
      vs.voxelType = voxelType;
      vs.state.assign(state.begin(), state.end());
    }
    if (markDirty && options_.writeBackIntervalMs > 0) c.dirty = true;
  }

  replication::Connection& conn_;
  domains::ChunksAPI* chunksApi_;  // may be null (offline / tests): no hydrate/write-back
  std::string appId_;
  Options options_;
  std::unordered_map<ChunkCoord, ChunkData, ChunkCoordHash> chunks_;
  std::int64_t lastWriteBackMs_ = 0;
};

}  // namespace crowdy::session
