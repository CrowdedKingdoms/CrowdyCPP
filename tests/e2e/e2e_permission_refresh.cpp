// Replication API behavior: runtime permission changes take effect on live
// sessions. When an owner revokes a player's access, spatial sends start
// drawing UNAUTHORIZED within the server's permission-cache window; a
// re-grant restores them. SLOW suite (cache TTL waits) — gated on
// CROWDY_E2E_SLOW. See
// https://docs.crowdedkingdoms.com/replication-api/operations.
#include <atomic>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

// Poll a voxel send until it is either accepted (no error / self-echo) or
// rejected with UNAUTHORIZED, up to budgetMs. Returns true when it settles
// into the wanted state (accepted=true wants acceptance).
bool voxelSettles(e2e::Player& p, const wire::ChunkCoord& chunk, const core::ActorUuid& uuid,
                  bool wantAccepted, int budgetMs) {
  const auto start = std::chrono::steady_clock::now();
  std::uint8_t seq = 0;
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(budgetMs)) {
    auto outcome = p.conn->sendVoxelUpdateAndWait(chunk, uuid, 1, 1, 1, 7, Bytes(), 8,
                                                  wire::DecayRate::None, 1500);
    (void)seq;
    if (wantAccepted && outcome.acknowledged && !outcome.error.has_value()) return true;
    if (!wantAccepted && outcome.error.has_value() &&
        outcome.error.value() == wire::ErrorCode::Unauthorized)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return false;
}

}  // namespace

int main() try {
  e2e::requireFlag("CROWDY_E2E_SLOW");
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto p = e2e::provisionPlayer(cfg, "permref");
  e2e::connectUdp(p, cfg);

  const wire::ChunkCoord chunk{100600, 0, 100600};
  const auto uuid = core::generateActorUuid();

  E2E_SUBTEST("granted player: voxel sends are accepted");
  E2E_CHECK(voxelSettles(p, chunk, uuid, /*wantAccepted=*/true, 30000));

  E2E_SUBTEST("owner revokes access -> sends become UNAUTHORIZED within the cache window");
  {
    graphql::JVal revoke;
    revoke["appId"] = cfg.appId;
    revoke["userId"] = p.userId;
    // RevokeAppAccess takes appId + userId (see the operation).
    e2e::owner(cfg).admin().appAccess().revoke(cfg.appId, p.userId);
  }
  // The server caches (user, app, chunk) verdicts for a few minutes; allow a
  // generous window for the revocation to propagate.
  std::printf("waiting for revocation to take effect (up to 6 min)...\n");
  E2E_CHECK(voxelSettles(p, chunk, uuid, /*wantAccepted=*/false, 360000));

  E2E_SUBTEST("owner re-grants -> sends are accepted again");
  {
    const std::string tierId = e2e::ensureEntitledTier(cfg, cfg.appId);
    graphql::JVal grant;
    grant["appId"] = cfg.appId;
    grant["userId"] = p.userId;
    grant["tierId"] = tierId;
    e2e::owner(cfg).admin().appAccess().grant(grant);
  }
  std::printf("waiting for re-grant to take effect (up to 6 min)...\n");
  // A fresh chunk avoids any lingering negative cache on the first one.
  const wire::ChunkCoord chunk2{100601, 0, 100600};
  E2E_CHECK(voxelSettles(p, chunk2, uuid, /*wantAccepted=*/true, 360000));

  p.conn->disconnect();
  std::puts("e2e_permission_refresh OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
