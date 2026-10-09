// Distance-limited channel messages over native UDP (Buddy v0.35.0): the owner makes a channel
// with four members, each holding an actor at its own chunk. A sends
// Connection::sendRangedChannelMessage from its chunk: radius 5 reaches only B, exactly 5 chunks
// away (C, a Chebyshev neighbour at ~5.66, does not); 6 adds C; 7 adds D. A gets no echo.
// Members receive the ordinary channel notification. Mirrors CrowdyJS's
// test/e2e/ranged-channel-message.test.mjs.
#include <atomic>
#include <cstring>
#include <set>
#include <string>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

struct Member {
  e2e::Player player;
  core::ActorUuid uuid;
  wire::ChunkCoord chunk;
};

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);

  // Far from every other suite's actors, so nothing else is in range.
  const std::int64_t bx = 300900, bz = 300900;
  Member members[4] = {
      {e2e::provisionPlayer(cfg, "ranged-a"), core::generateActorUuid(), {bx, 0, bz}},
      {e2e::provisionPlayer(cfg, "ranged-b"), core::generateActorUuid(), {bx + 3, 4, bz}},
      {e2e::provisionPlayer(cfg, "ranged-c"), core::generateActorUuid(), {bx + 4, 4, bz}},
      {e2e::provisionPlayer(cfg, "ranged-d"), core::generateActorUuid(), {bx, 0, bz + 7}},
  };

  E2E_SUBTEST("owner creates the channel and adds the four members");
  graphql::JVal channelInput;
  channelInput["appId"] = cfg.appId;
  channelInput["name"] = "e2e-ranged-" + e2e::runSuffix();
  channelInput["membershipPolicy"] = "invite";
  channelInput["membersCanSend"] = true;
  graphql::Json channel = e2e::ownerGame(cfg).channels().create(channelInput);
  const std::string channelGroupId = channel["groupId"].asString();
  E2E_CHECK(!channelGroupId.empty());
  const std::int64_t channelId = channel["groupId"].asBigInt();
  for (auto& m : members) e2e::ownerGame(cfg).channels().addMember(channelGroupId, m.player.userId);

  E2E_SUBTEST("connect and register each member's actor");
  std::atomic<int> seen[4] = {};
  std::string expected;  // the payload of the send in flight
  for (int i = 0; i < 4; ++i) {
    e2e::connectUdp(members[i].player, cfg);
    Handlers h;
    h.channelMessage = [&, i](const ChannelNotification& n) {
      if (n.channelId == channelId && asStringView(n.payload) == expected) ++seen[i];
    };
    members[i].player.conn->setHandlers(std::move(h));
  }
  const std::uint8_t pose[] = {1};
  auto keepAlive = [&] {
    for (auto& m : members)
      E2E_CHECK(m.player.conn->sendActorUpdate({m.chunk, m.uuid, Bytes(pose, 1), 0}).ok());
  };
  keepAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // grid windows + membership
  keepAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto whoReceives = [&](std::uint32_t maxDistance) {
    for (auto& s : seen) s = 0;
    expected = "ranged-" + std::to_string(maxDistance) + "-" + e2e::runSuffix();
    keepAlive();
    E2E_CHECK(members[0]
                  .player.conn
                  ->sendRangedChannelMessage(channelId, members[0].uuid, asBytes(expected),
                                             members[0].chunk, maxDistance)
                  .ok());
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < until) {
      for (auto& m : members) m.player.conn->poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::set<int> got;
    for (int i = 0; i < 4; ++i)
      if (seen[i].load() > 0) got.insert(i);
    return got;
  };

  E2E_SUBTEST("radius 5 reaches only the member exactly 5 chunks away");
  E2E_CHECK((whoReceives(5) == std::set<int>{1}));
  E2E_SUBTEST("radius 6 adds the Chebyshev neighbour at ~5.66");
  E2E_CHECK((whoReceives(6) == std::set<int>{1, 2}));
  E2E_SUBTEST("radius 7 adds the member 7 chunks away");
  E2E_CHECK((whoReceives(7) == std::set<int>{1, 2, 3}));

  E2E_CHECK(e2e::ownerGame(cfg).channels().remove(channelGroupId).asBool());
  for (auto& m : members) m.player.conn->disconnect();
  std::puts("e2e_ranged_channel OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
