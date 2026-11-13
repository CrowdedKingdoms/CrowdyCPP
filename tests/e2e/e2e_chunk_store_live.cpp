// Data-structure e2e: the ChunkStore against a live deployment — durable
// hydration (ensureAround), optimistic edits with write-back persistence,
// worldgen seeding (seed + flush), streaming-world eviction (pruneBeyond)
// that persists dirty chunks before dropping them, and a write-back refused
// in a closed wilderness being dropped after one attempt.
#include <array>
#include <cstring>
#include <vector>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

namespace {

std::array<std::uint8_t, kChunkVolume> patternVoxels(std::uint8_t seed) {
  std::array<std::uint8_t, kChunkVolume> v{};
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::uint8_t>((i * 31 + seed) & 0x7f);
  return v;
}

}  // namespace

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto p = e2e::provisionPlayer(cfg, "chunkstore");
  e2e::connectUdp(p, cfg);

  WorldSessionConfig sc;
  sc.appId = cfg.appId;
  sc.hostHeartbeatIntervalMs = 0;
  sc.chunks.writeBackIntervalMs = 200;  // fast write-back for the test
  WorldSession session(p.conn, p.game.get(), sc);

  // One base for every run: since ck-api v2.70.0 a whole-chunk write supersedes the edits earlier
  // runs recorded there, so the chunk reads back exactly as written (0.84.0's e2e had to move to a
  // fresh base each run).
  const wire::ChunkCoord base{510000, 0, 510000};

  E2E_SUBTEST("durable write then ensureAround hydrates byte-identical");
  const auto stored = patternVoxels(1);
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["coordinates"] = domains::ChunkRef{base.x, base.y, base.z}.toInput();
    input["voxels"] = core::base64Encode(Bytes(stored.data(), stored.size()));
    p.game->chunks().update(input);
  }
  E2E_CHECK(session.chunks().ensureAround(base, 1) >= 1);
  const ChunkData* hydrated = session.chunks().find(base);
  E2E_CHECK(hydrated != nullptr);
  E2E_CHECK(hydrated->storedOnServer);
  E2E_CHECK(std::memcmp(hydrated->grid(), stored.data(), stored.size()) == 0);
  E2E_CHECK(session.chunks().voxelTypeAt(base, 1, 0, 0) == stored[1]);

  E2E_SUBTEST("optimistic setVoxel is echoed and recorded by the server");
  const std::uint8_t meta[] = {0x77};
  bool echoed = false;
  auto edit = session.chunks().setVoxel(base, 3, 4, 5, 99, Bytes(meta, 1), session.actorUuid(), true,
                                        [&](const EditOutcome& o) { echoed = o.result == EditResult::Echoed; });
  E2E_CHECK(edit.ok());
  E2E_CHECK(session.chunks().voxelTypeAt(base, 3, 4, 5) == 99);
  E2E_CHECK(session.chunks().pendingWriteBacks() == 0);  // a realtime edit is not written back
  const auto start = std::chrono::steady_clock::now();
  while (!echoed && std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
    session.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  E2E_CHECK(echoed);
  // A second, independent session reads the recorded edit (the server records it within ~2 s).
  {
    WorldSessionConfig sc2;
    sc2.appId = cfg.appId;
    sc2.hostHeartbeatIntervalMs = 0;
    bool seen = false;
    const auto readStart = std::chrono::steady_clock::now();
    while (!seen && std::chrono::steady_clock::now() - readStart < std::chrono::seconds(15)) {
      WorldSession fresh(p.conn, p.game.get(), sc2);
      E2E_CHECK(fresh.chunks().ensureAround(base, 0) >= 1);
      seen = fresh.chunks().voxelTypeAt(base, 3, 4, 5) == 99;
      if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    E2E_CHECK(seen);
  }

  E2E_SUBTEST("a whole-chunk write supersedes the recorded edit (ck-api v2.70.0)");
  {
    graphql::JVal input;
    input["appId"] = cfg.appId;
    input["coordinates"] = domains::ChunkRef{base.x, base.y, base.z}.toInput();
    input["voxels"] = core::base64Encode(Bytes(stored.data(), stored.size()));
    p.game->chunks().update(input);
    WorldSessionConfig sc3;
    sc3.appId = cfg.appId;
    sc3.hostHeartbeatIntervalMs = 0;
    WorldSession fresh(p.conn, p.game.get(), sc3);
    E2E_CHECK(fresh.chunks().ensureAround(base, 0) >= 1);
    const ChunkData* rewritten = fresh.chunks().find(base);
    E2E_CHECK(rewritten != nullptr);
    E2E_CHECK(std::memcmp(rewritten->grid(), stored.data(), stored.size()) == 0);
    E2E_CHECK(fresh.chunks().voxelTypeAt(base, 3, 4, 5) == stored[3 + 4 * 16 + 5 * 256]);
    E2E_CHECK(rewritten->storedAtMs > 0);  // the voxels write
    E2E_CHECK(rewritten->rowAtMs >= rewritten->storedAtMs);
  }

  E2E_SUBTEST("seed() worldgen write-back reaches the durable store");
  const wire::ChunkCoord genCoord{base.x + 5, 0, base.z};
  const auto generated = patternVoxels(2);
  session.chunks().seed(genCoord, generated);
  {
    const ChunkFlushResult flushed = session.chunks().flushAndWait();
    E2E_CHECK(flushed.persisted >= 1);
    E2E_CHECK(flushed.dropped.empty());
  }
  {
    WorldSessionConfig sc2;
    sc2.appId = cfg.appId;
    sc2.hostHeartbeatIntervalMs = 0;
    WorldSession fresh(p.conn, p.game.get(), sc2);
    E2E_CHECK(fresh.chunks().ensureAround(genCoord, 0) >= 1);
    const ChunkData* c = fresh.chunks().find(genCoord);
    E2E_CHECK(c != nullptr);
    E2E_CHECK(std::memcmp(c->grid(), generated.data(), generated.size()) == 0);
  }

  E2E_SUBTEST("pruneBeyond keeps a dirty chunk until it is written back, then evicts it");
  const wire::ChunkCoord farCoord{base.x + 50, 0, base.z};
  session.chunks().seed(farCoord, patternVoxels(3));  // dirty, far away
  session.chunks().pruneBeyond(base, 10);
  E2E_CHECK(session.chunks().find(farCoord) != nullptr);  // its write-back is queued
  const auto pruneStart = std::chrono::steady_clock::now();
  while (session.chunks().find(farCoord)->dirty &&
         std::chrono::steady_clock::now() - pruneStart < std::chrono::seconds(15)) {
    session.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  E2E_CHECK(!session.chunks().find(farCoord)->dirty);
  E2E_CHECK(session.chunks().pruneBeyond(base, 10) >= 1);
  E2E_CHECK(session.chunks().find(farCoord) == nullptr);
  {
    WorldSessionConfig sc2;
    sc2.appId = cfg.appId;
    sc2.hostHeartbeatIntervalMs = 0;
    WorldSession fresh(p.conn, p.game.get(), sc2);
    E2E_CHECK(fresh.chunks().ensureAround(farCoord, 0) >= 1);
    E2E_CHECK(fresh.chunks().find(farCoord) != nullptr);
  }

  E2E_SUBTEST("closed wilderness: the refused write-back is dropped after one attempt");
  {
    // The org admin closes the app's wilderness (UpdateApp selects
    // wildernessWritesOpen); within ~15 s every replica refuses the player's chunk
    // writes there with FORBIDDEN. E2E_CHECK exits without unwinding, so the
    // observations are recorded first and the wilderness is reopened before any
    // of them is checked.
    auto& own = e2e::owner(cfg);
    graphql::JVal closeInput;
    closeInput["wildernessWritesOpen"] = false;
    const graphql::Json closed = own.admin().apps().update(cfg.appId, closeInput);
    const bool readBackClosed =
        closed["wildernessWritesOpen"].isBool() && !closed["wildernessWritesOpen"].asBool();

    std::vector<ChunkWriteBackFailure> reported;
    session.chunks().onWriteBackFailed(
        [&](const ChunkWriteBackFailure& failure) { reported.push_back(failure); });
    const wire::ChunkCoord refusedCoord{base.x + 90, 0, base.z};
    ChunkFlushResult refused;
    const auto closeStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - closeStart < std::chrono::seconds(45)) {
      session.chunks().seed(refusedCoord, patternVoxels(4));
      refused = session.chunks().flushAndWait();
      if (!refused.dropped.empty()) break;
      std::this_thread::sleep_for(std::chrono::seconds(2));  // not yet on this replica
    }
    const ChunkData* refusedChunk = session.chunks().find(refusedCoord);
    const bool refusedNotDirty = refusedChunk != nullptr && !refusedChunk->dirty;
    const std::size_t pendingAfter = session.chunks().pendingWriteBacks();

    graphql::JVal openInput;
    openInput["wildernessWritesOpen"] = true;
    bool reopened = false;
    try {
      reopened = own.admin().apps().update(cfg.appId, openInput)["wildernessWritesOpen"].asBool();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "reopening the wilderness failed: %s\n", e.what());
    }
    E2E_CHECK(reopened);
    E2E_CHECK(readBackClosed);
    E2E_CHECK(refused.dropped.size() == 1);
    const ChunkWriteBackFailure& failure = refused.dropped.front();
    E2E_CHECK(failure.reason == ChunkWriteBackDrop::Refused);
    E2E_CHECK(failure.attempts == 1);
    E2E_CHECK(!failure.error.errors.empty() && failure.error.errors.front().code == "FORBIDDEN");
    E2E_CHECK(reported.size() == 1 && reported.front().coord == refusedCoord);
    E2E_CHECK(refusedNotDirty);
    E2E_CHECK(pendingAfter == 0);
    std::printf("   refused after %d attempt(s): %s\n", failure.attempts,
                failure.error.errors.front().message.c_str());

    // Reopened, the same chunk writes back again (once every replica has it).
    bool persistedAgain = false;
    const auto openStart = std::chrono::steady_clock::now();
    while (!persistedAgain &&
           std::chrono::steady_clock::now() - openStart < std::chrono::seconds(45)) {
      session.chunks().seed(refusedCoord, patternVoxels(5));
      persistedAgain = session.chunks().flushAndWait().persisted == 1;
      if (!persistedAgain) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    E2E_CHECK(persistedAgain);
    session.chunks().onWriteBackFailed({});
  }

  E2E_SUBTEST("onChunkChanged fires for realtime merges");
  int changes = 0;
  session.chunks().onChunkChanged([&](const ChunkData&) { ++changes; });
  session.chunks().setVoxel(base, 1, 1, 1, 7, Bytes(), session.actorUuid());
  E2E_CHECK(changes >= 1);

  session.dispose();
  std::puts("e2e_chunk_store_live OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
