#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/game_session.hpp"
#include "crowdy/graphql/http.hpp"
#include "test_util.hpp"

using namespace crowdy;

// crowdy::startGameSession: a session from an app token the game already holds (signInWithMinecraft,
// an organization's mint, a portal exchange), built the way the SDK's own sign-in path builds one.

namespace {

const std::string kAppToken(64, 'a');

std::string bearer(const graphql::HttpRequest& request) {
  for (const auto& [name, value] : request.headers) {
    if (name == "Authorization") return value;
  }
  return {};
}

class AssignmentHttp final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::HttpRequest> requests;
  int refuseFirst = 0;
  int assignments = 0;

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    if (request.body.find("ServerWithLeastClients") != std::string::npos) {
      ++assignments;
      if (refuseFirst > 0) {
        --refuseFirst;
        return {200,
                R"({"errors":[{"message":"No available servers found","extensions":{"code":"UNAVAILABLE"}}]})"};
      }
      return {200,
              R"({"data":{"serverWithLeastClients":{"serverId":"server-1","ip4":"127.0.0.1","ip6":"","clientPort":39001,"status":"READY","peers":[],"clients":"0","cpuPeakPct":0,"updatedAt":"","createdAt":""}}})"};
    }
    return {200, R"({"data":{"ok":true}})"};
  }
};

domains::AppTokenResponse appToken() {
  domains::AppTokenResponse token;
  token.token = kAppToken;
  token.gameTokenId = "9007199254740993";  // past a double's integers: never narrowed
  token.appId = "107209219611648";
  token.expiresAt = "2030-01-01T00:00:00.000Z";
  token.gameApiUrl = "https://ck-or-1.game.invalid";
  token.gameApiWsUrl = "wss://ck-or-1.game.invalid";
  token.discoveryUrl = "https://ck.game.invalid";
  return token;
}

GameSessionOptions manualOptions(const std::shared_ptr<AssignmentHttp>& http) {
  GameSessionOptions options;
  options.client.transport = http;
  options.replication.manualPump = true;
  options.replication.sessionReadyWaitMs = 0;
  options.connectTimeoutMs = 2000;
  return options;
}

void testReplicationTokenOf() {
  const auto info = replicationTokenOf(appToken());
  CHECK_EQ(info.token, kAppToken);
  CHECK_EQ(info.gameTokenId, std::int64_t{9007199254740993});
  CHECK_EQ(info.expiresAtEpochMs, std::int64_t{1893456000000});

  auto noExpiry = appToken();
  noExpiry.expiresAt.clear();
  CHECK_EQ(replicationTokenOf(noExpiry).expiresAtEpochMs, std::int64_t{0});

  auto org = appToken();
  org.token = "cko_" + std::string(60, 'b');
  CHECK(replicationTokenOf(org).token.empty());
  auto shortToken = appToken();
  shortToken.token = "abc";
  CHECK(replicationTokenOf(shortToken).token.empty());
  auto badId = appToken();
  badId.gameTokenId = "not-a-number";
  CHECK(replicationTokenOf(badId).token.empty());
  CHECK_EQ(replicationTokenOf(badId).gameTokenId, std::int64_t{0});
}

void testGameClientConfig() {
  ClientConfig base;
  base.httpUrl = "https://identity.invalid";
  base.wsEndpoint = "/stale";
  base.timeoutMs = 1234;
  const ClientConfig game = gameClientConfig(appToken(), base, "https://fallback.invalid");
  CHECK_EQ(game.httpUrl, std::string("https://ck-or-1.game.invalid"));
  CHECK_EQ(game.wsUrl, std::string("wss://ck-or-1.game.invalid"));
  CHECK_EQ(game.discoveryUrl, std::string("https://ck.game.invalid"));
  CHECK(game.wsEndpoint.empty());
  CHECK_EQ(game.timeoutMs, 1234L);

  // A token without its own origins (an older API, a local stack) goes to the fallback, then
  // to what the base had.
  domains::AppTokenResponse bare = appToken();
  bare.gameApiUrl = std::nullopt;
  bare.gameApiWsUrl = std::nullopt;
  bare.discoveryUrl = std::nullopt;
  const ClientConfig fallback = gameClientConfig(bare, base, "https://fallback.invalid");
  CHECK_EQ(fallback.httpUrl, std::string("https://fallback.invalid"));
  CHECK_EQ(fallback.discoveryUrl, std::string("https://fallback.invalid"));
  CHECK(fallback.wsUrl.empty());
  const ClientConfig own = gameClientConfig(bare, base);
  CHECK_EQ(own.httpUrl, std::string("https://identity.invalid"));
}

void testStartsFromTheToken() {
  auto http = std::make_shared<AssignmentHttp>();
  int statusCalls = 0;
  GameSessionOptions options = manualOptions(http);
  options.handlers.status = [&](replication::ConnState) { ++statusCalls; };
  GameSession session = startGameSession(appToken(), std::move(options));
  CHECK(session.ok());
  CHECK(session.client != nullptr);
  CHECK(session.connection != nullptr);
  CHECK(session.connection->state() == replication::ConnState::Connected);
  CHECK_EQ(session.appId(), std::int64_t{107209219611648});
  CHECK(!session.paused());

  // The per-game client is on the app's datacenter and holds the app token.
  CHECK_EQ(session.client->getToken(), kAppToken);
  CHECK_EQ(session.client->graphqlClient().endpoint(),
           std::string("https://ck-or-1.game.invalid/graphql"));
  CHECK_EQ(session.client->websocketEndpoint(), std::string("wss://ck-or-1.game.invalid/graphql"));
  CHECK(session.client->canRediscover());
  CHECK_EQ(http->requests.size(), std::size_t{1});
  CHECK(http->requests.at(0).body.find("ServerWithLeastClients") != std::string::npos);
  CHECK_EQ(http->requests.at(0).url, std::string("https://ck-or-1.game.invalid/graphql"));
  CHECK_EQ(bearer(http->requests.at(0)), "Bearer " + kAppToken);

  // The replication connection is signed with the token and is the client's active one.
  CHECK(session.client->replication().activeConnection() == session.connection);
  CHECK_EQ(session.client->activeAppId(), std::string("107209219611648"));
  CHECK_EQ(session.connection->assignmentSnapshot().ip4, std::string("127.0.0.1"));
  // No handler ran inside startGameSession; the caller's poll() delivers them.
  CHECK_EQ(statusCalls, 0);
  session.connection->poll();
  CHECK(statusCalls > 0);

  auto paused = appToken();
  paused.runtimeGate = domains::AppRuntimeGate{"GRACE", "wallet"};
  GameSession pausedSession = startGameSession(paused, manualOptions(std::make_shared<AssignmentHttp>()));
  CHECK(pausedSession.ok());
  CHECK(pausedSession.paused());
}

void testRefusesWhatIsNotAnAppToken() {
  const auto refused = [](domains::AppTokenResponse token) {
    auto http = std::make_shared<AssignmentHttp>();
    GameSession session = startGameSession(token, manualOptions(http));
    CHECK(!session.ok());
    CHECK(session.status.code == Errc::InvalidArgument);
    CHECK(session.client == nullptr);
    CHECK(session.connection == nullptr);
    CHECK(http->requests.empty());
  };
  domains::AppTokenResponse empty = appToken();
  empty.token.clear();
  refused(empty);
  domains::AppTokenResponse org = appToken();
  org.token = "cko_" + std::string(60, 'c');
  refused(org);
  domains::AppTokenResponse shortToken = appToken();
  shortToken.token = std::string(63, 'a');
  refused(shortToken);
  domains::AppTokenResponse noApp = appToken();
  noApp.appId.clear();
  refused(noApp);
  domains::AppTokenResponse badTokenId = appToken();
  badTokenId.gameTokenId = "x1";
  refused(badTokenId);
}

void testAssignmentRetriesAndFailures() {
  auto http = std::make_shared<AssignmentHttp>();
  http->refuseFirst = 1;
  GameSessionOptions options = manualOptions(http);
  options.connectAttempts = 2;
  options.connectRetryDelayMs = 1;
  GameSession retried = startGameSession(appToken(), std::move(options));
  CHECK(retried.ok());
  CHECK_EQ(http->assignments, 2);

  auto refusing = std::make_shared<AssignmentHttp>();
  refusing->refuseFirst = 5;
  GameSession failed = startGameSession(appToken(), manualOptions(refusing));
  CHECK(!failed.ok());
  CHECK(failed.status.code != Errc::InvalidArgument);
  // The client and the Failed connection are kept, so the caller can retry connect().
  CHECK(failed.client != nullptr);
  CHECK(failed.connection != nullptr);
  CHECK(failed.connection->state() == replication::ConnState::Failed);
  CHECK_EQ(refusing->assignments, 1);

  // Not ready in time: Timeout, with everything kept.
  auto slow = std::make_shared<AssignmentHttp>();
  GameSessionOptions slowOptions = manualOptions(slow);
  slowOptions.replication.sessionReadyWaitMs = 60000;
  slowOptions.connectTimeoutMs = 30;
  GameSession late = startGameSession(appToken(), std::move(slowOptions));
  CHECK(late.status.code == Errc::Timeout);
  CHECK(late.connection != nullptr);
  CHECK(late.connection->state() == replication::ConnState::Connecting);

  // connectTimeoutMs 0 returns once assigned, still Connecting.
  auto quick = std::make_shared<AssignmentHttp>();
  GameSessionOptions quickOptions = manualOptions(quick);
  quickOptions.replication.sessionReadyWaitMs = 60000;
  quickOptions.connectTimeoutMs = 0;
  GameSession assigned = startGameSession(appToken(), std::move(quickOptions));
  CHECK(assigned.ok());
  CHECK(assigned.connection->state() == replication::ConnState::Connecting);
}

}  // namespace

int main() {
  testReplicationTokenOf();
  testGameClientConfig();
  testStartsFromTheToken();
  testRefusesWhatIsNotAnAppToken();
  testAssignmentRetriesAndFailures();
  std::puts("game_session_test: ok");
  return 0;
}
