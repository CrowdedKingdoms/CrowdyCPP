#include <chrono>
#include <cstdint>
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
  /// What a refused assignment answers; the default names no wait.
  std::string refusal =
      R"({"errors":[{"message":"No available servers found","extensions":{"code":"UNAVAILABLE"}}]})";

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    requests.push_back(request);
    if (request.body.find("ServerWithLeastClients") != std::string::npos) {
      ++assignments;
      if (refuseFirst > 0) {
        --refuseFirst;
        return {200, refusal};
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
  token.applicationApiUrl = "https://ck-or-1.game.invalid";
  token.applicationApiWsUrl = "wss://ck-or-1.game.invalid";
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
  bare.applicationApiUrl = std::nullopt;
  bare.applicationApiWsUrl = std::nullopt;
  bare.discoveryUrl = std::nullopt;
  const ClientConfig fallback = gameClientConfig(bare, base, "https://fallback.invalid");
  CHECK_EQ(fallback.httpUrl, std::string("https://fallback.invalid"));
  CHECK_EQ(fallback.discoveryUrl, std::string("https://fallback.invalid"));
  CHECK(fallback.wsUrl.empty());
  const ClientConfig own = gameClientConfig(bare, base);
  CHECK_EQ(own.httpUrl, std::string("https://identity.invalid"));
}

// A token built by hand with only the deprecated gameApi* members still routes (until they go on
// January 1, 2027).
void testATokenWithOnlyTheGameApiNamesStillRoutes() {
  domains::AppTokenResponse token = appToken();
  token.applicationApiUrl = std::nullopt;
  token.applicationApiWsUrl = std::nullopt;
  token.gameApiUrl = "https://ck-va-1.game.invalid";
  token.gameApiWsUrl = "wss://ck-va-1.game.invalid";
  ClientConfig base;
  const ClientConfig game = gameClientConfig(token, base, "");
  CHECK_EQ(game.httpUrl, std::string("https://ck-va-1.game.invalid"));
  CHECK_EQ(game.wsUrl, std::string("wss://ck-va-1.game.invalid"));
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
  // The refusal keeps its code (0.95.0); this one named no wait.
  CHECK(failed.failure.failed());
  CHECK_EQ(static_cast<int>(failed.failure.error), static_cast<int>(failed.status.code));
  CHECK_EQ(failed.failure.code, std::string("UNAVAILABLE"));
  CHECK_EQ(failed.failure.retryAfterMs, 0);
  CHECK_EQ(failed.failure.message, std::string("No available servers found"));
  CHECK(!failed.failure.platformBusy());
  CHECK(!retried.failure.failed());  // the attempt that succeeded leaves none

  // PLATFORM_BUSY with a wait: the caller is told to wait, how long, and that it may try
  // again. With attempts left, the next one waits as long as the API asked.
  auto busy = std::make_shared<AssignmentHttp>();
  busy->refuseFirst = 5;
  busy->refusal =
      R"({"errors":[{"message":"The platform is busy. Try again shortly.","extensions":{"code":"PLATFORM_BUSY","retryable":true,"blame":"PLATFORM","retryAfterMs":250}}]})";
  GameSession told = startGameSession(appToken(), manualOptions(busy));
  CHECK(!told.ok());
  CHECK(told.failure.platformBusy());
  CHECK_EQ(told.failure.code, std::string("PLATFORM_BUSY"));
  CHECK_EQ(told.failure.retryAfterMs, 250);
  CHECK_EQ(told.connection->lastAssignmentFailure().retryAfterMs, 250);
  // The game server does what it was told, on the connection it was left.
  busy->refuseFirst = 0;
  CHECK(told.connection->connect().ok());
  CHECK(!told.connection->lastAssignmentFailure().failed());

  auto waited = std::make_shared<AssignmentHttp>();
  waited->refuseFirst = 1;
  waited->refusal = busy->refusal;
  GameSessionOptions patient = manualOptions(waited);
  patient.connectAttempts = 2;
  patient.connectRetryDelayMs = 1;
  const auto asked = std::chrono::steady_clock::now();
  GameSession second = startGameSession(appToken(), std::move(patient));
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - asked);
  CHECK(second.ok());
  CHECK_EQ(waited->assignments, 2);
  CHECK(took.count() >= 250);  // the API's wait, where the caller's own was 1 ms
  CHECK(!second.failure.failed());

  // The API's wait is a server's word, so it counts up to a ceiling (kMaxRetryAfterWaitMs,
  // 30 s) and no further. A hint of days, of nothing, and below nothing: the waits a retry
  // makes are recorded instead of slept, so the first case does not take 30 s.
  struct Hint {
    const char* retryAfterMs;  // as the answer's JSON carries it
    std::int64_t reported;     // GameSession::failure.retryAfterMs
    std::int64_t waited;       // with connectRetryDelayMs 7
  };
  for (const Hint& hint : {Hint{"86400000000", kMaxRetryAfterWaitMs, kMaxRetryAfterWaitMs},
                           Hint{"9223372036854775807", kMaxRetryAfterWaitMs, kMaxRetryAfterWaitMs},
                           Hint{"18446744073709551615", kMaxRetryAfterWaitMs, kMaxRetryAfterWaitMs},
                           Hint{"1e300", kMaxRetryAfterWaitMs, kMaxRetryAfterWaitMs},
                           Hint{"-1e300", 0, 7},
                           Hint{"250.9", 250, 250},
                           Hint{"30001", kMaxRetryAfterWaitMs, kMaxRetryAfterWaitMs},
                           Hint{"30000", 30000, 30000},
                           Hint{"10000", 10000, 10000},
                           Hint{"0", 0, 7},
                           Hint{"-1", 0, 7},
                           Hint{"-86400000000", 0, 7}}) {
    auto hinting = std::make_shared<AssignmentHttp>();
    hinting->refuseFirst = 2;
    hinting->refusal =
        std::string(R"({"errors":[{"message":"busy","extensions":{"code":"PLATFORM_BUSY","retryable":true,"blame":"PLATFORM","retryAfterMs":)") +
        hint.retryAfterMs + "}}]}";
    GameSessionOptions bounded = manualOptions(hinting);
    bounded.connectAttempts = 3;
    bounded.connectRetryDelayMs = 7;
    std::vector<std::int64_t> waits;
    bounded.sleep = [&waits](std::int64_t ms) { waits.push_back(ms); };
    const auto began = std::chrono::steady_clock::now();
    GameSession session = startGameSession(appToken(), std::move(bounded));
    const auto spent =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    CHECK(session.ok());
    CHECK_EQ(hinting->assignments, 3);
    CHECK_EQ(waits.size(), 2u);
    for (const std::int64_t wait : waits) CHECK_EQ(wait, hint.waited);
    CHECK(spent.count() < 20000);  // nothing slept on the hint itself

    // What the caller is handed is bounded the same way.
    hinting->assignments = 0;
    hinting->refuseFirst = 5;
    GameSession refusedOnce = startGameSession(appToken(), manualOptions(hinting));
    CHECK(!refusedOnce.ok());
    CHECK_EQ(refusedOnce.failure.retryAfterMs, hint.reported);
    CHECK_EQ(refusedOnce.connection->lastAssignmentFailure().retryAfterMs, hint.reported);
  }
  // The function the wait is made with, and the one under it.
  replication::AssignmentFailure handMade;
  handMade.retryAfterMs = 86'400'000'000LL;  // a provider of the caller's own, unbounded
  CHECK_EQ(connectRetryWaitMs(2000, handMade), kMaxRetryAfterWaitMs);
  handMade.retryAfterMs = -5;
  CHECK_EQ(connectRetryWaitMs(2000, handMade), 2000);
  CHECK_EQ(connectRetryWaitMs(-3, handMade), 0);
  handMade.retryAfterMs = 0;
  CHECK_EQ(connectRetryWaitMs(40000, handMade), 40000);  // the caller's own delay is the caller's
  CHECK_EQ(kMaxRetryAfterWaitMs, 30000);
  CHECK_EQ(retryWaitMs(0), 0);
  CHECK_EQ(retryWaitMs(-1), 0);
  CHECK_EQ(retryWaitMs(INT64_MIN), 0);
  CHECK_EQ(retryWaitMs(1), 1);
  CHECK_EQ(retryWaitMs(200), 200);
  CHECK_EQ(retryWaitMs(30000), 30000);
  CHECK_EQ(retryWaitMs(30001), 30000);
  CHECK_EQ(retryWaitMs(INT64_MAX), 30000);

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
  testATokenWithOnlyTheGameApiNamesStillRoutes();
  testStartsFromTheToken();
  testRefusesWhatIsNotAnAppToken();
  testAssignmentRetriesAndFailures();
  std::puts("game_session_test: ok");
  return 0;
}
