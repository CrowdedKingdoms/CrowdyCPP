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

// Org-managed players (CrowdyJS 18.16.0, ck-api v2.52.0, dev only). The device holds the
// player's app token and never the organization's org token; the admin domain wraps the
// organization's administration and no org-token mint.

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
    R"("gameTokenId":"9","appId":"7","expiresAt":"2026-10-22T00:00:00.000Z","gameApiUrl":null,)"
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
  CHECK_EQ(rig.auth->token(), std::string(64, 'c'));
  CHECK_EQ(rig.http->requests.size(), std::size_t{1});
  const graphql::Json body = bodyOf(rig.http->requests[0]);
  CHECK(body["query"].asString().find("exchangeManagedPlayerAssertion(input: $input)") !=
        std::string::npos);
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
  testRegisterSigningKeySendsJwkTextAndRefusesAPrivateKey();
  testRevokeTokenSendsEndSessionsOnlyWhenAsked();
  std::printf("managed_players_test passed\n");
  return 0;
}
