#include <memory>
#include <string>
#include <vector>

#include "crowdy/core/crypto.hpp"
#include "crowdy/domains/admin.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

// Org-managed players (CrowdyJS 18.16.0 / ck-api v2.52.0; the addendum, CrowdyJS 18.17.0 /
// ck-api v2.53.0). The device holds the player's app token and never the organization's org
// token; the admin domain wraps the organization's administration, and neither the org-token
// mint nor the addendum's acceptance.

namespace {

class RecordingHttp final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  std::string reply = R"({"data":{"ok":true}})";
  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    return {200, reply};
  }
};

struct Rig {
  std::shared_ptr<RecordingHttp> http = std::make_shared<RecordingHttp>();
  std::shared_ptr<graphql::AuthState> auth = std::make_shared<graphql::AuthState>();
  std::shared_ptr<graphql::GraphQLClient> gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http, auth);
};

graphql::Json bodyOf(const graphql::HttpRequest& request) {
  return graphql::Json::parse(request.body);
}

const char* kTokenReply =
    R"({"data":{"exchangeManagedPlayerAssertion":{"token":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",)"
    R"("gameTokenId":"9","userId":"9007199254740993","appId":"7","expiresAt":"2026-10-22T00:00:00.000Z","gameApiUrl":null,)"
    R"("gameApiWsUrl":null,"discoveryUrl":null,"launchUrl":null,"runtimeGate":null}}})";

void testAdoptStoresTheAppToken() {
  Rig rig;
  domains::PortalAPI portal(rig.gql, rig.auth, core::unavailableCrypto());
  domains::AppTokenResponse response;
  response.token = std::string(64, 'a');
  CHECK(portal.adoptAppToken(response).ok());
  CHECK_EQ(rig.auth->token(), std::string(64, 'a'));
  CHECK(rig.http->requests.empty());
}

void testAdoptRefusesAnOrgTokenAndAnEmptyOne() {
  Rig rig;
  domains::PortalAPI portal(rig.gql, rig.auth, core::unavailableCrypto());
  domains::AppTokenResponse orgToken;
  orgToken.token = "cko_" + std::string(64, 'b');
  CHECK(portal.adoptAppToken(orgToken).code == Errc::InvalidArgument);
  domains::AppTokenResponse empty;
  CHECK(portal.adoptAppToken(empty).code == Errc::InvalidArgument);
  CHECK(rig.auth->token().empty());
}

void testExchangeTradesTheAssertionAndStoresTheToken() {
  Rig rig;
  rig.http->reply = kTokenReply;
  domains::PortalAPI portal(rig.gql, rig.auth, core::unavailableCrypto());
  const domains::AppTokenResponse r = portal.exchangeManagedPlayerAssertion("7", "h.p.s");
  CHECK_EQ(r.token, std::string(64, 'c'));
  // The user the token signs in (0.95.0, Application API v2.90.0): past a double's
  // integers, so never narrowed, and readable as the int64 callAs takes.
  CHECK_EQ(r.userId, std::string("9007199254740993"));
  CHECK(r.userIdInt64() == std::optional<std::int64_t>(9007199254740993LL));
  CHECK_EQ(rig.auth->token(), std::string(64, 'c'));
  CHECK_EQ(rig.http->requests.size(), std::size_t{1});
  const graphql::Json body = bodyOf(rig.http->requests[0]);
  CHECK(body["query"].asString().find("exchangeManagedPlayerAssertion(input: $input)") !=
        std::string::npos);
  CHECK(body["query"].asString().find(" token gameTokenId userId appId ") != std::string::npos);
  CHECK_EQ(body["variables"]["input"]["appId"].asString(), std::string("7"));
  CHECK_EQ(body["variables"]["input"]["assertion"].asString(), std::string("h.p.s"));

  Rig async;
  async.http->reply = kTokenReply;
  domains::PortalAPI asyncPortal(async.gql, async.auth, core::unavailableCrypto());
  bool called = false;
  asyncPortal.exchangeManagedPlayerAssertionAsync(
      "7", "h.p.s", [&](graphql::GraphQLOutcome out, domains::AppTokenResponse got) {
        called = true;
        CHECK(out.ok());
        CHECK_EQ(got.token, std::string(64, 'c'));
      });
  CHECK(called);
  CHECK_EQ(async.auth->token(), std::string(64, 'c'));
}

void testAdminFieldsAndArguments() {
  Rig rig;
  domains::ManagedPlayersAPI players(rig.gql);
  players.list("3", "acct-", 20);
  players.get("3", "acct-1");
  players.endSessions("3", "acct-1");
  players.remove("3", "acct-1");
  players.settings("3");
  players.signingKeys("3");
  players.revokeSigningKey("3", "11");
  players.appModsEnabled("7");
  players.setAppMods("7", false);
  graphql::JVal provision;
  provision["orgId"] = "3";
  provision["players"] = graphql::JVal::array({graphql::JVal::object({{"externalId", "acct-1"}})});
  players.provision(provision);

  const std::vector<std::pair<std::string, std::string>> expected = {
      {"managedPlayers(", R"({"first":20,"orgId":"3","search":"acct-"})"},
      {"managedPlayer(", R"({"externalId":"acct-1","orgId":"3"})"},
      {"endManagedPlayerSessions(", R"({"externalId":"acct-1","orgId":"3"})"},
      {"deleteManagedPlayer(", R"({"externalId":"acct-1","orgId":"3"})"},
      {"orgManagedPlayerSettings(", R"({"orgId":"3"})"},
      {"orgSigningKeys(", R"({"orgId":"3"})"},
      {"revokeOrgSigningKey(", R"({"keyId":"11","orgId":"3"})"},
      {"app(", R"({"appId":"7"})"},
      {"setAppManagedPlayerMods(", R"({"appId":"7","enabled":false})"},
      {"provisionManagedPlayers(", R"({"input":{"orgId":"3","players":[{"externalId":"acct-1"}]}})"},
  };
  CHECK_EQ(rig.http->requests.size(), expected.size());
  for (std::size_t i = 0; i < expected.size() && i < rig.http->requests.size(); ++i) {
    const graphql::Json body = bodyOf(rig.http->requests[i]);
    CHECK(body["query"].asString().find(expected[i].first) != std::string::npos);
    CHECK_EQ(body["variables"].dump(), expected[i].second);
  }
  // settings() reads where the organization stands on the Org-Managed Players Addendum.
  const std::string settingsQuery = bodyOf(rig.http->requests[4])["query"].asString();
  for (const char* field : {"termsVersion", "termsUrl", "termsAccepted", "viewerMayAcceptTerms"}) {
    CHECK(settingsQuery.find(field) != std::string::npos);
  }
  // Nothing in the SDK accepts the addendum: a person does, in Studio.
  for (const auto& request : rig.http->requests) {
    CHECK(bodyOf(request)["query"].asString().find("acceptManagedPlayerTerms") == std::string::npos);
  }
}

// 0.95.0 (Application API v2.90.0): up to 100 players deleted in one call, each external
// id answered with its outcome; and the organization's fleet limits in its settings
// (Application API v2.89.0).
void testRemoveManyAndTheFleetLimits() {
  Rig rig;
  rig.http->reply =
      R"({"data":{"deleteManagedPlayers":{"deleted":1,"notFound":1,"held":1,"players":[)"
      R"({"externalId":"acct-1","outcome":"DELETED"},{"externalId":"gone","outcome":"NOT_FOUND"},)"
      R"({"externalId":"held-1","outcome":"LEGAL_HOLD"}]}}})";
  domains::ManagedPlayersAPI players(rig.gql);
  const graphql::Json done = players.removeMany("3", {"acct-1", "gone", "held-1", "acct-1"});
  CHECK_EQ(rig.http->requests.size(), std::size_t{1});
  const graphql::Json body = bodyOf(rig.http->requests[0]);
  const std::string query = body["query"].asString();
  CHECK(query.find("mutation DeleteManagedPlayers($input: DeleteManagedPlayersInput!)") != std::string::npos);
  CHECK(query.find("deleteManagedPlayers(input: $input)") != std::string::npos);
  for (const char* field : {"deleted", "notFound", "held", "externalId", "outcome"}) {
    CHECK(query.find(field) != std::string::npos);
  }
  // Every id as it was given, in order: the API deletes one sent twice once.
  CHECK_EQ(body["variables"].dump(),
           std::string(R"({"input":{"externalIds":["acct-1","gone","held-1","acct-1"],"orgId":"3"}})"));
  CHECK_EQ(done["deleted"].asInt64(), 1);
  CHECK_EQ(done["notFound"].asInt64(), 1);
  CHECK_EQ(done["held"].asInt64(), 1);  // a held player does not fail the call
  CHECK_EQ(done["players"].size(), 3u);
  const gen::ManagedPlayerDeletion expected[] = {gen::ManagedPlayerDeletion::DELETED,
                                                 gen::ManagedPlayerDeletion::NOT_FOUND,
                                                 gen::ManagedPlayerDeletion::LEGAL_HOLD};
  std::size_t at = 0;
  done["players"].forEach([&](const graphql::Json& player) {
    const auto outcome = gen::managedPlayerDeletionFromString(player["outcome"].asString());
    CHECK(outcome.has_value());
    if (outcome && at < 3) CHECK(*outcome == expected[at]);
    ++at;
  });
  CHECK_EQ(at, std::size_t{3});

  bool called = false;
  players.removeManyAsync("3", {"acct-1"}, [&](graphql::GraphQLOutcome out) {
    called = true;
    CHECK(out.ok());
    CHECK_EQ(out.data["deleted"].asInt64(), 1);
  });
  CHECK(called);
  CHECK_EQ(rig.http->requests.size(), std::size_t{2});
  CHECK_EQ(bodyOf(rig.http->requests[1])["variables"].dump(),
           std::string(R"({"input":{"externalIds":["acct-1"],"orgId":"3"}})"));

  // 1 to 100 ids a call. None, and 101, are refused before any request, and the refusal
  // says how many it was given and names none of them.
  CHECK_EQ(domains::ManagedPlayersAPI::kRemoveManyMax, std::size_t{100});
  std::vector<std::string> hundred;
  for (int i = 0; i < 100; ++i) hundred.push_back("player-name-" + std::to_string(i));
  players.removeMany("3", hundred);
  CHECK_EQ(rig.http->requests.size(), std::size_t{3});
  CHECK_EQ(bodyOf(rig.http->requests[2])["variables"]["input"]["externalIds"].size(), 100u);
  std::vector<std::string> tooMany = hundred;
  tooMany.push_back("player-name-100");
  const std::vector<std::string> none;
  for (const std::vector<std::string>* ids : {static_cast<const std::vector<std::string>*>(&tooMany), &none}) {
    const std::vector<std::string>& given = *ids;
#ifndef CROWDY_NO_EXCEPTIONS
    bool threw = false;
    try {
      players.removeMany("3", given);
    } catch (const graphql::CrowdyError& e) {
      threw = true;
      CHECK_EQ(std::string(e.code()), std::string("INVALID_ARGUMENT"));
      const std::string said = e.what();
      CHECK(said.find("1 to 100 external ids") != std::string::npos);
      CHECK(said.find("was given " + std::to_string(given.size())) != std::string::npos);
      CHECK(said.find("player-name") == std::string::npos);
    }
    CHECK(threw);
#else
    CHECK(players.removeMany("3", given).isNull());
#endif
    called = false;
    players.removeManyAsync("3", given, [&](graphql::GraphQLOutcome out) {
      called = true;
      CHECK(out.status.code == Errc::InvalidArgument);
      CHECK(out.errorMessage.find("was given " + std::to_string(given.size())) != std::string::npos);
      CHECK(out.errorMessage.find("player-name") == std::string::npos);
    });
    CHECK(called);
  }
  CHECK_EQ(rig.http->requests.size(), std::size_t{3});

  // settings() selects the organization's fleet limits.
  rig.http->reply =
      R"({"data":{"orgManagedPlayerSettings":{"orgId":"3","enabled":true,"fleetSessionsEnabled":true,"maxFleetSessions":512,"maxFleetPlayers":256}}})";
  const graphql::Json settings = players.settings("3");
  const std::string settingsQuery = bodyOf(rig.http->requests.back())["query"].asString();
  for (const char* field : {"fleetSessionsEnabled", "maxFleetSessions", "maxFleetPlayers"}) {
    CHECK(settingsQuery.find(field) != std::string::npos);
  }
  CHECK(settings["fleetSessionsEnabled"].asBool());
  CHECK_EQ(settings["maxFleetSessions"].asInt64(), 512);
  CHECK_EQ(settings["maxFleetPlayers"].asInt64(), 256);
}

void testRegisterSigningKeySendsJwkTextAndRefusesAPrivateKey() {
  Rig rig;
  domains::ManagedPlayersAPI players(rig.gql);
  graphql::JVal input;
  input["orgId"] = "3";
  input["kid"] = "k1";
  input["algorithm"] = "EdDSA";
  input["issuer"] = "https://id.example";
  input["publicJwk"] = graphql::JVal::object({{"kty", "OKP"}, {"crv", "Ed25519"}, {"x", "abc"}});
  players.registerSigningKey(input);
  CHECK_EQ(rig.http->requests.size(), std::size_t{1});
  const graphql::Json sent = bodyOf(rig.http->requests[0])["variables"]["input"];
  CHECK(sent["publicJwk"].isString());
  CHECK_EQ(sent["publicJwk"].asString(), std::string(R"({"crv":"Ed25519","kty":"OKP","x":"abc"})"));

  graphql::JVal privateKey = input;
  privateKey["publicJwk"] = R"({"kty":"OKP","crv":"Ed25519","x":"abc","d":"secret"})";
  CHECK(domains::ManagedPlayersAPI::carriesPrivateJwkMember(privateKey));
#ifndef CROWDY_NO_EXCEPTIONS
  bool threw = false;
  try {
    players.registerSigningKey(privateKey);
  } catch (const graphql::CrowdyError& e) {
    threw = true;
    CHECK_EQ(std::string(e.code()), std::string("INVALID_ARGUMENT"));
  }
  CHECK(threw);
#endif
  bool called = false;
  players.registerSigningKeyAsync(privateKey, [&](graphql::GraphQLOutcome out) {
    called = true;
    CHECK(out.status.code == Errc::InvalidArgument);
  });
  CHECK(called);
  CHECK_EQ(rig.http->requests.size(), std::size_t{1});
}

void testRevokeTokenSendsEndSessionsOnlyWhenAsked() {
  Rig rig;
  domains::OrganizationsAPI orgs(rig.gql);
  orgs.revokeToken("5");
  orgs.revokeToken("5", true);
  CHECK_EQ(rig.http->requests.size(), std::size_t{2});
  CHECK_EQ(bodyOf(rig.http->requests[0])["variables"].dump(), std::string(R"({"orgTokenId":"5"})"));
  CHECK_EQ(bodyOf(rig.http->requests[1])["variables"].dump(),
           std::string(R"({"endSessions":true,"orgTokenId":"5"})"));
}

}  // namespace

int main() {
  testAdoptStoresTheAppToken();
  testAdoptRefusesAnOrgTokenAndAnEmptyOne();
  testExchangeTradesTheAssertionAndStoresTheToken();
  testAdminFieldsAndArguments();
  testRemoveManyAndTheFleetLimits();
  testRegisterSigningKeySendsJwkTextAndRefusesAPrivateKey();
  testRevokeTokenSendsEndSessionsOnlyWhenAsked();
  std::printf("managed_players_test passed\n");
  return 0;
}
