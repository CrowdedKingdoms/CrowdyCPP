// WorldSession end-to-end: two players drive the session layer against a live
// deployment — A's send loop and B's remote-actor registry see each other,
// and an optimistic voxel edit from A merges into B's chunk cache with the
// server's time (0.84.0), and A's echo settles it. With
// CROWDY_E2E_EXPECT_BOUND_ACK set, the servers must have acknowledged the bound
// control frames (CONTROL_BOUND_ACK, replication servers v0.46.0).
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

namespace {

std::unique_ptr<WorldSession> makeSession(e2e::Player& p, const e2e::E2eConfig& cfg) {
  WorldSessionConfig sc;
  sc.appId = cfg.appId;
  sc.self.sendHz = 5;
  sc.actors.staleAfterMs = 15000;
  sc.hostHeartbeatIntervalMs = 3000;
  return std::make_unique<WorldSession>(p.conn, p.game.get(), sc);
}

}  // namespace

int main() {
  auto cfg = e2e::requireConfig(/*needSecondPlayer=*/true);
  auto a = e2e::signIn(cfg, cfg.email);
  auto b = e2e::signIn(cfg, cfg.email2);
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  auto sa = makeSession(a, cfg);
  auto sb = makeSession(b, cfg);

  const wire::ChunkCoord chunk{3000, 0, 3000};
  struct Pose {
    float x = 0, y = 0, z = 0;
  };
  Pose poseA{1, 2, 3}, poseB{4, 5, 6};

  E2E_CHECK(sa->join(chunk, Bytes(reinterpret_cast<const std::uint8_t*>(&poseA), sizeof(poseA)))
                .ok());
  E2E_CHECK(sb->join(chunk, Bytes(reinterpret_cast<const std::uint8_t*>(&poseB), sizeof(poseB)))
                .ok());

  // Tick both sessions until each sees the other.
  const auto start = std::chrono::steady_clock::now();
  bool mutualVisibility = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
    poseA.x += 0.1f;  // keep state changing so sends keep flowing
    sa->self().setState(Bytes(reinterpret_cast<const std::uint8_t*>(&poseA), sizeof(poseA)));
    sa->tick();
    sb->tick();
    if (sa->actors().find(sb->actorUuid()) && sb->actors().find(sa->actorUuid())) {
      mutualVisibility = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  E2E_CHECK(mutualVisibility);

  const RemoteActor* remoteA = sb->actors().find(sa->actorUuid());
  E2E_CHECK(remoteA != nullptr);
  E2E_CHECK(remoteA->state().size() == sizeof(Pose));

  // Optimistic voxel edit from A appears in B's chunk cache.
  const std::uint8_t meta[] = {9};
  E2E_CHECK(sa->chunks().setVoxel(chunk, 4, 5, 6, 42, Bytes(meta, 1), sa->actorUuid()).ok());
  const auto voxelStart = std::chrono::steady_clock::now();
  bool voxelMerged = false;
  while (std::chrono::steady_clock::now() - voxelStart < std::chrono::seconds(10)) {
    sa->tick();
    sb->tick();
    const ChunkData* c = sb->chunks().find(chunk);
    if (c && c->typeAt(voxelIndex(4, 5, 6)) == 42) {
      voxelMerged = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  E2E_CHECK(voxelMerged);

  // B holds the edit with the server's time, near this machine's clock.
  const std::int64_t wallNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
  const std::int64_t confirmed = sb->chunks().confirmedAtMs(chunk, 4, 5, 6);
  std::printf("confirmed at %lld (local clock %lld)\n", static_cast<long long>(confirmed),
              static_cast<long long>(wallNow));
  E2E_CHECK(confirmed > wallNow - 120000 && confirmed < wallNow + 120000);

  // A's own echo settles its edit.
  const auto echoStart = std::chrono::steady_clock::now();
  while (sa->chunks().editsInFlight() > 0 &&
         std::chrono::steady_clock::now() - echoStart < std::chrono::seconds(10)) {
    sa->tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  E2E_CHECK(sa->chunks().editsInFlight() == 0);
  E2E_CHECK(sa->chunks().voxelTypeAt(chunk, 4, 5, 6) == 42);

  const auto acks = a.conn->stats().boundControlAcks;
  std::printf("bound control acks: %llu\n", static_cast<unsigned long long>(acks));
  if (std::getenv("CROWDY_E2E_EXPECT_BOUND_ACK")) E2E_CHECK(acks >= 1);

  sa->dispose();
  sb->dispose();
  std::puts("e2e_world_session OK");
  return 0;
}
