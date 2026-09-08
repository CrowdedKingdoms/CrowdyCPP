// Mirrors CrowdyJS's two-client-leave e2e on the native transport (Buddy
// v0.25.x, CrowdyCPP 0.30.0). Players A and B register in one chunk; B
// disconnects; within a few seconds the game server stops considering B's
// actor present and A must receive exactly ONE ActorLeftNotification carrying
// B's uuid and last chunk (reason STALE) -- instead of guessing after the
// stores' staleAfterMs reap. See
// https://docs.crowdedkingdoms.com/replication-api/wire-formats.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  auto a = e2e::provisionPlayer(cfg, "leave-a");
  auto b = e2e::provisionPlayer(cfg, "leave-b");
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  const wire::ChunkCoord chunk{100400, 0, 100400};
  E2E_CHECK(e2e::warmUp(*a.conn, chunk));
  E2E_CHECK(e2e::warmUp(*b.conn, chunk));

  std::atomic<int> leftB{0}, leftA{0};
  std::uint8_t lastReason = 9;
  wire::ChunkCoord lastChunk{};
  Handlers ha;
  ha.actorLeft = [&](const SpatialNotification& n, std::uint8_t reason) {
    if (std::memcmp(n.uuid, uuidB.data(), wire::kUuidSize) == 0) {
      ++leftB;
      lastReason = reason;
      lastChunk = n.chunk;
    } else if (std::memcmp(n.uuid, uuidA.data(), wire::kUuidSize) == 0) {
      ++leftA;
    }
  };
  a.conn->setHandlers(std::move(ha));

  const std::uint8_t pose[] = {1};
  for (int i = 0; i < 3; ++i) {
    E2E_CHECK(a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 2}).ok());
    E2E_CHECK(b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 2}).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    a.conn->poll();
    b.conn->poll();
  }

  E2E_SUBTEST("B disconnects; A receives one ActorLeftNotification for B");
  b.conn->disconnect();
  // Keep A fresh so A is not the one announced; Buddy removes B ~5 s after its
  // last update, plus the peer hop if the two sit on different Buddies.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (std::chrono::steady_clock::now() < deadline) {
    E2E_CHECK(a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 2}).ok());
    for (int i = 0; i < 10; ++i) {
      a.conn->poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (leftB.load() >= 1) break;
  }
  // Drain a moment longer to prove there is no second announcement.
  for (int i = 0; i < 20; ++i) {
    a.conn->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::printf("actorLeft: forB=%d forA=%d reason=%u chunk=(%lld,%lld,%lld)\n", leftB.load(),
              leftA.load(), static_cast<unsigned>(lastReason),
              static_cast<long long>(lastChunk.x), static_cast<long long>(lastChunk.y),
              static_cast<long long>(lastChunk.z));
  E2E_CHECK(leftB.load() == 1);
  E2E_CHECK(leftA.load() == 0);
  E2E_CHECK(lastReason == 0);
  E2E_CHECK(lastChunk.x == chunk.x && lastChunk.y == chunk.y && lastChunk.z == chunk.z);

  a.conn->disconnect();
  std::puts("e2e_two_client_leave OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unexpected exception: %s\n", e.what());
    return 1;
  }
}
