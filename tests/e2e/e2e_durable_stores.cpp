// Data-structure e2e: durable stores against a live deployment —
// SaveStateStore/AvatarStateStore round-trips, and FileUuidStore actor
// identity surviving a reconnect (the remote side sees the same actor).
#include <atomic>
#include <cstdio>
#include <cstring>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::session;

int main() try {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  auto p = e2e::provisionPlayer(cfg, "durable");

  E2E_SUBTEST("SaveStateStore patch stays local until explicit save");
  SaveStateStore save(*p.game, cfg.appId);
  const std::uint8_t blob[] = {1, 2, 3, 4, 5};
  save.save(Bytes(blob, sizeof(blob)));
  {
    SaveStateStore fresh(*p.game, cfg.appId);
    const auto& loaded = fresh.load();
    E2E_CHECK(loaded.size() == sizeof(blob));
    E2E_CHECK(std::memcmp(loaded.data(), blob, sizeof(blob)) == 0);
    const std::uint8_t patchBytes[] = {9, 9};
    fresh.patch(1, Bytes(patchBytes, sizeof(patchBytes)));
    E2E_CHECK(fresh.dirty());
    const auto local = fresh.snapshot();
    E2E_CHECK(local[0] == 1 && local[1] == 9 && local[2] == 9 &&
              local[3] == 4);

    SaveStateStore beforeSave(*p.game, cfg.appId);
    const auto& persisted = beforeSave.load();
    E2E_CHECK(persisted.size() == sizeof(blob));
    E2E_CHECK(std::memcmp(persisted.data(), blob, sizeof(blob)) == 0);

    fresh.save();
    E2E_CHECK(!fresh.dirty());
    E2E_CHECK(fresh.lastSavedAt().has_value());
  }
  {
    SaveStateStore afterSave(*p.game, cfg.appId);
    const auto& loaded = afterSave.load();
    E2E_CHECK(loaded.size() == sizeof(blob));
    E2E_CHECK(loaded[0] == 1 && loaded[1] == 9 && loaded[2] == 9 &&
              loaded[3] == 4);
  }

  E2E_SUBTEST("AvatarStateStore identity + per-app state round-trips");
  graphql::JVal avatarInput;
  avatarInput["name"] = "durable-e2e-" + e2e::runSuffix();
  graphql::Json avatar = p.game->avatars().create(avatarInput);
  const std::string avatarId = avatar["id"].asString(avatar["avatarId"].asString());
  E2E_CHECK(!avatarId.empty());
  AvatarStateStore avatarStore(*p.game, cfg.appId, avatarId);
  const std::uint8_t identityBytes[] = {0xaa, 0xbb};
  const std::uint8_t appBytes[] = {0xcc};
  avatarStore.setIdentityState(Bytes(identityBytes, sizeof(identityBytes)));
  avatarStore.setAppState(Bytes(appBytes, sizeof(appBytes)));
  {
    AvatarStateStore fresh(*p.game, cfg.appId, avatarId);
    fresh.load();
    E2E_CHECK(fresh.identityState().size() == 2 && fresh.identityState()[0] == 0xaa);
    E2E_CHECK(fresh.appState().size() == 1 && fresh.appState()[0] == 0xcc);
  }

  E2E_SUBTEST("FileUuidStore identity survives reconnect");
  auto sender = e2e::provisionPlayer(cfg, "durable-uuid");
  e2e::connectUdp(p, cfg);
  e2e::connectUdp(sender, cfg);

  const std::string uuidPath = "/tmp/crowdycpp-e2e-uuid-" + e2e::runSuffix();
  FileUuidStore uuidStore(uuidPath);
  const core::ActorUuid stable = ensureActorUuid(uuidStore);
  E2E_CHECK(ensureActorUuid(uuidStore) == stable);  // persisted, not re-minted

  const wire::ChunkCoord chunk{520000, 0, 520000};
  std::atomic<int> seenStable{0};
  replication::Handlers hs;
  hs.actorUpdate = [&](const replication::SpatialNotification& n) {
    if (std::memcmp(n.uuid, stable.data(), 32) == 0) ++seenStable;
  };
  p.conn->setHandlers(std::move(hs));

  // The observer needs spatial presence near the chunk to receive fan-out.
  const auto observerUuid = core::generateActorUuid();
  const std::uint8_t pose[] = {5};
  bool seen1 = e2e::retryUntil(
      [&] {
        p.conn->sendActorUpdate({chunk, observerUuid, Bytes(pose, 1), 8});
        sender.conn->sendActorUpdate({chunk, stable, Bytes(pose, 1), 8});
      },
      [&] {
        p.conn->poll();
        return seenStable.load() >= 1;
      });
  E2E_CHECK(seen1);

  // Reconnect the sender; the persisted uuid keeps the same identity.
  sender.conn->disconnect();
  e2e::connectUdp(sender, cfg);
  FileUuidStore reload(uuidPath);
  const core::ActorUuid after = ensureActorUuid(reload);
  E2E_CHECK(after == stable);
  const int baseline = seenStable.load();
  bool seen2 = e2e::retryUntil(
      [&] {
        p.conn->sendActorUpdate({chunk, observerUuid, Bytes(pose, 1), 8});
        sender.conn->sendActorUpdate({chunk, after, Bytes(pose, 1), 8});
      },
      [&] {
        p.conn->poll();
        return seenStable.load() > baseline;
      });
  E2E_CHECK(seen2);

  std::remove(uuidPath.c_str());
  p.conn->disconnect();
  sender.conn->disconnect();
  std::puts("e2e_durable_stores OK");
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
