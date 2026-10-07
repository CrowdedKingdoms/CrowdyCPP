// Game Kit e2e: social (guilds, chat, parties).
//
// A guild is a team paired with a chat channel (guildCreate over the platform
// teams). The suite verifies membership add via team join, guild chat
// published over the native UDP replication connection and received on
// another member's channelMessage handler, and the party helpers. See
// https://docs.crowdedkingdoms.com/game-api/teams-and-channels.
#include <atomic>
#include <cstdlib>

#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;
using namespace crowdy::replication;

int main() {
  try {
    auto cfg = e2e::requireConfig();

    E2E_SUBTEST("provision members and connect native UDP");
    auto a = e2e::provisionPlayer(cfg, "cs-a");  // guild leader
    auto b = e2e::provisionPlayer(cfg, "cs-b");  // joining member
    e2e::connectUdp(a, cfg);
    e2e::connectUdp(b, cfg);

    auto kitA = makeKit(*a.game, cfg.appId, a.conn.get());
    auto kitB = makeKit(*b.game, cfg.appId, b.conn.get());

    E2E_SUBTEST("guild team + chat channel created first (guildCreate)");
    const std::string guildName = "cs-" + e2e::runSuffix();
    KitGuildCreateOptions guildOptions;
    guildOptions.membershipPolicy = "open";  // so B can join directly
    KitGroupWithChannel guild = kitA.social().guildCreate(guildName, guildOptions);
    E2E_CHECK(!guild.teamId.empty());
    E2E_CHECK(!guild.channelId.empty());

    // Membership add via team join (B joins the team AND the chat channel).
    b.game->teams().join(guild.teamId);
    b.game->channels().join(guild.channelId);
    bool bOnRoster = false;
    kitA.social().guildRoster(guild).forEach([&](graphql::Json m) {
      if (m["userId"].asString() == b.userId ||
          std::to_string(m["userId"].asBigInt()) == b.userId)
        bOnRoster = true;
    });
    E2E_CHECK(bOnRoster);

    E2E_SUBTEST("guild chat over native UDP reaches another member");
    const std::string chatText = "guild hello " + e2e::runSuffix();
    std::atomic<int> chats{0};
    Handlers hb;
    const std::int64_t guildChannelId = std::strtoll(guild.channelId.c_str(), nullptr, 10);
    hb.channelMessage = [&](const ChannelNotification& n) {
      if (n.channelId == guildChannelId && asStringView(n.payload) == chatText) ++chats;
    };
    b.conn->setHandlers(std::move(hb));

    const auto uuidA = core::generateActorUuid();
    const auto uuidB = core::generateActorUuid();
    const wire::ChunkCoord chunk{400500, 0, 400000};
    const std::uint8_t pose[] = {1};
    auto keepAlive = [&] {
      (void)a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8});
      (void)b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8});
    };
    keepAlive();
    std::this_thread::sleep_for(std::chrono::seconds(1));  // membership propagation

    bool chatSeen = e2e::retryUntil(
        [&] {
          keepAlive();
          E2E_CHECK(kitA.social().chatSend(guildChannelId, chatText));
        },
        [&] {
          b.conn->poll();
          return chats.load() > 0;
        },
        /*attempts=*/40, /*perWaitMs=*/500);
    E2E_CHECK(chatSeen);

    E2E_SUBTEST("party helpers: create + invite roster");
    const std::string partyName = "cs-party-" + e2e::runSuffix();
    KitGroupWithChannel party = kitA.social().partyCreate(partyName);
    E2E_CHECK(!party.teamId.empty());
    kitA.social().partyInvite(party, b.userId);
    bool bInParty = false;
    kitA.social().partyMembers(party).forEach([&](graphql::Json m) {
      if (m["userId"].asString() == b.userId ||
          std::to_string(m["userId"].asBigInt()) == b.userId)
        bInParty = true;
    });
    E2E_CHECK(bInParty);
    auto found = kitB.social().partyFind(partyName);
    E2E_CHECK(found.has_value());
    E2E_CHECK(found->teamId == party.teamId);

    a.conn->disconnect();
    b.conn->disconnect();
    std::puts("e2e_kit_social OK");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "e2e_kit_social FAILED: %s\n", e.what());
    return 1;
  }
}
