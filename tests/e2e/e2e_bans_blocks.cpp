// Bans and blocks: channel and team bans through the SDK's own ban() / unban(), the refusals
// they cause read with graphql::groupMemberBannedOf, the bans nobody may make, and, with an
// operator account configured, an account block read with graphql::accountBlockedOf: a timed
// one that lapses by itself and an open-ended one that lasts until it is lifted.
// Reference: https://docs.crowdedkingdoms.com/game-api/groups
#include <chrono>
#include <ctime>
#include <thread>

#include "e2e_util.hpp"

using namespace crowdy;

namespace {

/// The errors a call was refused with; empty when it succeeded.
template <class Fn>
std::vector<graphql::GraphQLErrorDetail> refusalOf(Fn&& call) {
  try {
    call();
  } catch (const graphql::CrowdyGraphQLError& e) {
    return e.errors();
  }
  return {};
}

std::string isoIn(int seconds) {
  const std::time_t t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + std::chrono::seconds(seconds));
  std::tm utc{};
  gmtime_r(&t, &utc);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buf;
}

constexpr const char* kBlock =
    "mutation($u:BigInt!,$r:String!,$t:DateTime){ blockAccount(userId:$u, reason:$r, until:$t){ blocked blockedUntil } }";
constexpr const char* kUnblock = "mutation($u:BigInt!){ unblockAccount(userId:$u){ blocked } }";
constexpr const char* kMe = "{ me { userId } }";

int run() {
  auto cfg = e2e::requireConfig();
  e2e::requireOwner(cfg);
  // The owner creates the groups (an app may let only its admins create them), so owns them.
  auto& owner = e2e::ownerGame(cfg);
  const std::string ownerId = e2e::owner(cfg).graphqlClient().request(kMe)["me"]["userId"].asString();
  auto b = e2e::provisionPlayer(cfg, "bans-b");  // the one banned and blocked
  auto c = e2e::provisionPlayer(cfg, "bans-c");  // a plain member, later a manager

  E2E_SUBTEST("channel: a ban refuses joining and being added, survives leaving, and unban lifts it");
  graphql::JVal channelInput;
  channelInput["appId"] = cfg.appId;
  channelInput["name"] = "e2e-bans-" + e2e::runSuffix();
  channelInput["membershipPolicy"] = "open";
  const std::string channel = owner.channels().create(channelInput)["groupId"].asString();
  E2E_CHECK(!channel.empty());
  b.game->channels().join(channel);
  c.game->channels().join(channel);
  E2E_CHECK(owner.channels().ban(channel, b.userId, "e2e")["status"].asString() == "banned");
  E2E_CHECK(graphql::groupMemberBannedOf(refusalOf([&] { b.game->channels().join(channel); })));
  E2E_CHECK(graphql::groupMemberBannedOf(
      refusalOf([&] { owner.channels().addMember(channel, b.userId); })));
  (void)refusalOf([&] { b.game->channels().leave(channel); });
  E2E_CHECK(graphql::groupMemberBannedOf(refusalOf([&] { b.game->channels().join(channel); })));
  E2E_CHECK(owner.channels().unban(channel, b.userId).asBool());
  E2E_CHECK(!owner.channels().unban(channel, b.userId).asBool());
  E2E_CHECK(b.game->channels().join(channel)["status"].asString() == "active");

  E2E_SUBTEST("channel: no manage_members, no ban; and nobody bans themselves");
  {
    auto plain = refusalOf([&] { c.game->channels().ban(channel, b.userId); });
    E2E_CHECK(!plain.empty() && !graphql::groupMemberBannedOf(plain));
    E2E_CHECK(!refusalOf([&] { owner.channels().ban(channel, ownerId); }).empty());
  }
  (void)refusalOf([&] { owner.channels().remove(channel); });

  E2E_SUBTEST("team: banned players cannot join, request to join, or be added or approved");
  graphql::JVal teamInput;
  teamInput["appId"] = cfg.appId;
  teamInput["name"] = "E2E Bans " + e2e::runSuffix();
  teamInput["membershipPolicy"] = "request";
  const std::string team = owner.teams().create(teamInput)["groupId"].asString();
  E2E_CHECK(!team.empty());
  E2E_CHECK(owner.teams().ban(team, b.userId, "e2e")["status"].asString() == "banned");
  E2E_CHECK(graphql::groupMemberBannedOf(refusalOf([&] { b.game->teams().requestToJoin(team); })));
  E2E_CHECK(graphql::groupMemberBannedOf(refusalOf([&] { b.game->teams().join(team); })));
  E2E_CHECK(graphql::groupMemberBannedOf(refusalOf([&] { owner.teams().addMember(team, b.userId); })));
  E2E_CHECK(owner.teams().unban(team, b.userId).asBool());
  E2E_CHECK(b.game->teams().requestToJoin(team)["status"].asString() == "pending");
  E2E_CHECK(owner.teams().addMember(team, b.userId)["status"].asString() == "active");

  E2E_SUBTEST("team: a manager may ban a member but never the team's owner");
  {
    c.game->teams().requestToJoin(team);
    owner.teams().addMember(team, c.userId);
    graphql::JVal roleInput;
    roleInput["groupId"] = team;
    roleInput["roleName"] = "e2e-warden-" + e2e::runSuffix();
    roleInput["permissions"] = graphql::JVal::array({graphql::JVal("manage_members")});
    roleInput["rank"] = std::int64_t{5};
    const std::string roleId = owner.teams().createRole(roleInput)["groupRoleId"].asString();
    graphql::JVal rolesInput;
    rolesInput["groupId"] = team;
    rolesInput["userId"] = c.userId;
    rolesInput["roleIds"] = graphql::JVal::array({graphql::JVal(roleId)});
    owner.teams().setMemberRoles(rolesInput);
    E2E_CHECK(!refusalOf([&] { c.game->teams().ban(team, ownerId); }).empty());
    E2E_CHECK(c.game->teams().ban(team, b.userId)["status"].asString() == "banned");
    E2E_CHECK(owner.teams().unban(team, b.userId).asBool());
  }
  (void)refusalOf([&] { owner.teams().remove(team); });

  E2E_SUBTEST("account block (CROWDY_E2E_OPERATOR_EMAIL / _PASSWORD)");
  const std::string opEmail = e2e::envOr("CROWDY_E2E_OPERATOR_EMAIL");
  const std::string opPassword = e2e::envOr("CROWDY_E2E_OPERATOR_PASSWORD");
  if (opEmail.empty() || opPassword.empty()) {
    std::puts("   no operator account configured; the block half is skipped");
    std::puts("e2e_bans_blocks OK");
    return 0;
  }
  ClientConfig opCfg;
  opCfg.httpUrl = cfg.apiUrl;
  CrowdyClient op(std::move(opCfg));
  op.setToken(op.auth().login(opEmail, opPassword).token);
  const std::string password = e2e::derivePassword(b.email);
  auto signIn = [&] {
    ClientConfig fresh;
    fresh.httpUrl = cfg.apiUrl;
    CrowdyClient client(std::move(fresh));
    return refusalOf([&] { client.auth().login(b.email, password); });
  };

  {
    graphql::JVal vars;
    vars["u"] = b.userId;
    vars["r"] = "CrowdyCPP e2e: a timed block";
    vars["t"] = isoIn(20);
    E2E_CHECK(op.graphqlClient().request(kBlock, vars)["blockAccount"]["blocked"].asBool());
  }
  {
    auto refused = refusalOf([&] { b.identity->graphqlClient().request(kMe); });
    auto block = graphql::accountBlockedOf(refused);
    E2E_CHECK(block.has_value() || (!refused.empty() && refused.front().code == "UNAUTHENTICATED"));
    auto login = graphql::accountBlockedOf(signIn());
    E2E_CHECK(login.has_value() && !login->blockedUntil.empty());
  }
  // Sign-in is rate-limited: one attempt every 3 s until the 20 s block has lapsed.
  bool lapsed = false;
  for (int i = 0; i < 15 && !lapsed; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    lapsed = signIn().empty();
  }
  E2E_CHECK(lapsed);

  {
    graphql::JVal vars;
    vars["u"] = b.userId;
    vars["r"] = "CrowdyCPP e2e: until lifted";
    E2E_CHECK(op.graphqlClient().request(kBlock, vars)["blockAccount"]["blocked"].asBool());
    auto login = graphql::accountBlockedOf(signIn());
    E2E_CHECK(login.has_value() && login->blockedUntil.empty());
    graphql::JVal lift;
    lift["u"] = b.userId;
    E2E_CHECK(!op.graphqlClient().request(kUnblock, lift)["unblockAccount"]["blocked"].asBool());
    E2E_CHECK(signIn().empty());
  }

  std::puts("e2e_bans_blocks OK");
  return 0;
}

}  // namespace

int main() try {
  return run();
} catch (const std::exception& e) {
  std::fprintf(stderr, "exception: %s\n", e.what());
  return 1;
}
