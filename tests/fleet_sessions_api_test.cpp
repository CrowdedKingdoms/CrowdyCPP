// The Application API's part of a fleet session, on a stub HTTP transport in
// managed_players_test's manner: what each call sends, how its answer is read, the refusals a
// game server tells apart, and how an answer reaches the wire class. No network, except the
// last case (a loopback UDP server, where the build has OpenSSL and POSIX sockets), which
// shows a session that was told to move asking for a new server.
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "crowdy/core/clock.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/domains/admin.hpp"
#include "crowdy/domains/fleet_sessions.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/replication/fleet_session.hpp"
#include "test_util.hpp"

#ifdef CROWDY_TEST_FLEET_LOOPBACK
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace crowdy;
using domains::FleetSessionResponse;
using domains::FleetSessionsAPI;

namespace {

const std::string kOrgToken = "cko_" + std::string(48, 'o');
const std::string kFleetToken(64, 'f');

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
  Rig() { auth->setToken(kOrgToken); }
};

graphql::Json bodyOf(const graphql::HttpRequest& request) {
  return graphql::Json::parse(request.body);
}

std::string headerOf(const graphql::HttpRequest& request, const std::string& name) {
  for (const auto& [key, value] : request.headers) {
    if (key == name) return value;
  }
  return {};
}

/// A FleetSession answer under `field`, as the API sends it: BigInts as decimal strings.
std::string sessionReply(const std::string& field, const std::string& expiresAt,
                         const std::string& ip4 = "10.0.0.5", int clientPort = 7777,
                         const std::string& gameTokenId = "9001") {
  return R"({"data":{")" + field + R"(":{"token":")" + kFleetToken + R"(","gameTokenId":")" +
         gameTokenId + R"(","appId":"7","expiresAt":")" + expiresAt +
         R"(","maxPlayers":256,"server":{"serverId":"srv-1","ip4":")" + ip4 +
         R"(","ip6":"","clientPort":)" + std::to_string(clientPort) +
         R"(,"status":"ReadyForClients","clients":12}}}})";
}

std::string refusal(const std::string& code, const std::string& extra = {}) {
  return R"({"errors":[{"message":"refused","path":["field"],"extensions":{"code":")" + code + R"(")" +
         extra + R"(}}],"data":null})";
}

const char* kOpenedAt = "2026-11-26T00:00:00.000Z";
const char* kExpiresAt = "2026-11-27T00:00:00.000Z";
const std::int64_t kOpenedAtMs = core::parseIso8601Millis(kOpenedAt, 24);
const std::int64_t kExpiresAtMs = core::parseIso8601Millis(kExpiresAt, 24);

/// A clock that moves only when the test moves it.
struct ManualClock final : core::IClock {
  std::atomic<std::int64_t> epoch{0};
  std::atomic<std::int64_t> mono{50'000};
  std::int64_t epochMillis() const override { return epoch.load(); }
  std::int64_t monotonicMillis() const override { return mono.load(); }
  void advance(std::int64_t ms) {
    epoch.fetch_add(ms);
    mono.fetch_add(ms);
  }
};

// ---------------------------------------------------------------------------

void testOpenSendsTheAppAndReadsTheAnswer() {
  Rig rig;
  rig.http->reply = sessionReply("openFleetSession", kExpiresAt);
  FleetSessionsAPI api(rig.gql);
  const FleetSessionResponse opened = api.open("7");

  CHECK_EQ(rig.http->requests.size(), std::size_t{1});
  const graphql::Json body = bodyOf(rig.http->requests[0]);
  const std::string query = body["query"].asString();
  CHECK(query.find("mutation OpenFleetSession($input: OpenFleetSessionInput!)") != std::string::npos);
  CHECK(query.find("openFleetSession(input: $input)") != std::string::npos);
  for (const char* field : {"token", "gameTokenId", "appId", "expiresAt", "maxPlayers", "server",
                            "serverId", "ip4", "ip6", "clientPort", "status", "clients"}) {
    CHECK(query.find(field) != std::string::npos);
  }
  CHECK_EQ(body["variables"].dump(), std::string(R"({"input":{"appId":"7"}})"));
  // The bearer is the organization's org token: the fleet token is never one.
  CHECK_EQ(headerOf(rig.http->requests[0], "Authorization"), "Bearer " + kOrgToken);

  CHECK_EQ(opened.token, kFleetToken);
  CHECK_EQ(opened.gameTokenId, std::string("9001"));
  CHECK_EQ(opened.appId, std::string("7"));
  CHECK_EQ(opened.expiresAt, std::string(kExpiresAt));
  CHECK_EQ(opened.maxPlayers, 256);
  CHECK_EQ(opened.server.serverId, std::string("srv-1"));
  CHECK_EQ(opened.server.ip4, std::string("10.0.0.5"));
  CHECK_EQ(opened.server.clientPort, 7777);
  CHECK_EQ(opened.server.status, std::string("ReadyForClients"));
  CHECK_EQ(opened.server.clients, 12);
  CHECK(opened.gameTokenIdInt64() == std::optional<std::int64_t>(9001));
  CHECK(opened.appIdInt64() == std::optional<std::int64_t>(7));

  // The async twin sends the same request and reads the same answer; client.admin() has it.
  Rig async;
  async.http->reply = sessionReply("openFleetSession", kExpiresAt);
  domains::AdminAPI admin(async.gql);
  bool called = false;
  admin.fleetSessions().openAsync("7", [&](graphql::GraphQLOutcome out, FleetSessionResponse got) {
    called = true;
    CHECK(out.ok());
    CHECK_EQ(got.token, kFleetToken);
    CHECK_EQ(got.server.clientPort, 7777);
  });
  CHECK(called);
  CHECK_EQ(bodyOf(async.http->requests[0])["variables"].dump(), body["variables"].dump());
  CHECK_EQ(bodyOf(async.http->requests[0])["query"].asString(), query);

  // BigInts that arrive as JSON numbers are read too.
  const FleetSessionResponse numbers = FleetSessionResponse::fromJson(graphql::Json::parse(
      R"({"token":"t","gameTokenId":9001,"appId":7,"expiresAt":"x","maxPlayers":1,"server":null})"));
  CHECK_EQ(numbers.gameTokenId, std::string("9001"));
  CHECK_EQ(numbers.appId, std::string("7"));
  CHECK(numbers.server.ip4.empty());
}

void testRefreshNamesTheSessionAndItsServer() {
  Rig rig;
  rig.http->reply = sessionReply("refreshFleetSession", kExpiresAt, "10.0.0.9", 7001);
  FleetSessionsAPI api(rig.gql);

  // No server named: the answer's server is a new one to connect to.
  const FleetSessionResponse moved = api.refresh("7", "9001");
  CHECK_EQ(moved.server.ip4, std::string("10.0.0.9"));
  CHECK_EQ(moved.server.clientPort, 7001);
  CHECK_EQ(moved.token, kFleetToken);
  CHECK_EQ(moved.gameTokenId, std::string("9001"));
  // The server the session is connected to, so it stays there.
  api.refresh("7", "9001", "10.0.0.5", 7777);
  bool called = false;
  api.refreshAsync("7", "9001", [&](graphql::GraphQLOutcome out, FleetSessionResponse got) {
    called = true;
    CHECK(out.ok());
    CHECK_EQ(got.server.clientPort, 7001);
  });
  CHECK(called);
  called = false;
  api.refreshAsync("7", "9001", "10.0.0.5", 7777,
                   [&](graphql::GraphQLOutcome out, FleetSessionResponse) {
                     called = true;
                     CHECK(out.ok());
                   });
  CHECK(called);

  const std::string without = R"({"input":{"appId":"7","gameTokenId":"9001"}})";
  const std::string with =
      R"({"input":{"appId":"7","currentServer":{"clientPort":7777,"ip4":"10.0.0.5"},"gameTokenId":"9001"}})";
  CHECK_EQ(rig.http->requests.size(), std::size_t{4});
  const std::string expected[] = {without, with, without, with};
  for (std::size_t i = 0; i < 4 && i < rig.http->requests.size(); ++i) {
    const graphql::Json body = bodyOf(rig.http->requests[i]);
    const std::string query = body["query"].asString();
    CHECK(query.find("mutation RefreshFleetSession($input: RefreshFleetSessionInput!)") != std::string::npos);
    CHECK(query.find("refreshFleetSession(input: $input)") != std::string::npos);
    CHECK(query.find("maxPlayers") != std::string::npos);
    CHECK_EQ(body["variables"].dump(), expected[i]);
    CHECK_EQ(headerOf(rig.http->requests[i], "Authorization"), "Bearer " + kOrgToken);
  }
}

void testCloseAnswersWhetherASessionEnded() {
  Rig rig;
  FleetSessionsAPI api(rig.gql);
  rig.http->reply = R"({"data":{"closeFleetSession":true}})";
  CHECK(api.close("7", "9001"));
  // None by that id for this org token: already closed, or opened with another org token.
  rig.http->reply = R"({"data":{"closeFleetSession":false}})";
  CHECK(!api.close("7", "9001"));

  bool called = false;
  rig.http->reply = R"({"data":{"closeFleetSession":true}})";
  api.closeAsync("7", "9001", [&](graphql::GraphQLOutcome out, bool closed) {
    called = true;
    CHECK(out.ok());
    CHECK(closed);
  });
  CHECK(called);
  called = false;
  rig.http->reply = R"({"data":{"closeFleetSession":false}})";
  api.closeAsync("7", "9001", [&](graphql::GraphQLOutcome out, bool closed) {
    called = true;
    CHECK(out.ok());
    CHECK(!closed);
  });
  CHECK(called);
  // A refusal is not "closed".
  called = false;
  rig.http->reply = refusal("ORG_TOKEN_REQUIRED");
  api.closeAsync("7", "9001", [&](graphql::GraphQLOutcome out, bool closed) {
    called = true;
    CHECK(!out.ok());
    CHECK(!closed);
    CHECK(graphql::orgTokenRequiredOf(out.errors));
  });
  CHECK(called);

  CHECK_EQ(rig.http->requests.size(), std::size_t{5});
  for (const auto& request : rig.http->requests) {
    const graphql::Json body = bodyOf(request);
    CHECK(body["query"].asString().find("mutation CloseFleetSession($input: CloseFleetSessionInput!)") !=
          std::string::npos);
    CHECK(body["query"].asString().find("closeFleetSession(input: $input)") != std::string::npos);
    CHECK_EQ(body["variables"].dump(), std::string(R"({"input":{"appId":"7","gameTokenId":"9001"}})"));
  }
}

// The refusals a game server tells apart, by extensions.code.
void testRefusalsAreToldApart() {
  CHECK_EQ(graphql::kOrgTokenRequiredCode, std::string_view("ORG_TOKEN_REQUIRED"));
  CHECK_EQ(graphql::kFleetSessionLimitCode, std::string_view("FLEET_SESSION_LIMIT"));
  CHECK_EQ(graphql::kNotFoundCode, std::string_view("NOT_FOUND"));
  CHECK_EQ(graphql::kPlatformBusyCode, std::string_view("PLATFORM_BUSY"));

  Rig rig;
  FleetSessionsAPI api(rig.gql);
  const auto open = [&](const std::string& reply) {
    rig.http->reply = reply;
    graphql::GraphQLOutcome got;
    bool called = false;
    api.openAsync("7", [&](graphql::GraphQLOutcome out, FleetSessionResponse answer) {
      called = true;
      // A refused call's answer is empty, and no configuration comes of it.
      CHECK(answer.token.empty());
      CHECK(domains::fleetTokenOf(answer).token.empty());
      CHECK(domains::fleetConfigOf(answer).error() == Errc::InvalidArgument);
      got = std::move(out);
    });
    CHECK(called);
    CHECK(!got.ok());
    CHECK(got.kind == graphql::GraphQLErrorKind::GraphQL);
    CHECK_EQ(got.errors.size(), std::size_t{1});
    return got;
  };

  // The bearer was not an org token (a session token, an app token, the fleet token itself).
  graphql::GraphQLOutcome out = open(refusal("ORG_TOKEN_REQUIRED", R"(,"httpStatus":403,"retryable":false)"));
  CHECK(graphql::orgTokenRequiredOf(out.errors));
  CHECK(!graphql::fleetSessionLimitOf(out.errors));
  CHECK(!graphql::fleetSessionNotFoundOf(out.errors));
  CHECK(!graphql::platformBusyOf(out.errors).has_value());
  CHECK(!out.errors[0].retryable);
  CHECK(out.errors[0].httpStatus == std::optional<int>(403));

  // The organization's sessions are at their limit.
  out = open(refusal("FLEET_SESSION_LIMIT", R"(,"httpStatus":403)"));
  CHECK(graphql::fleetSessionLimitOf(out.errors));
  CHECK(!graphql::orgTokenRequiredOf(out.errors));
  CHECK(!graphql::platformBusyOf(out.errors).has_value());

  // The platform is busy, with how long to wait.
  out = open(refusal("PLATFORM_BUSY", R"(,"blame":"PLATFORM","retryable":true,"retryAfterMs":1500)"));
  const auto busy = graphql::platformBusyOf(out.errors);
  CHECK(busy.has_value());
  CHECK(busy->retryAfterMs == std::optional<std::int64_t>(1500));
  CHECK(out.errors[0].retryAfterMs == std::optional<std::int64_t>(1500));
  CHECK(out.errors[0].retryable);
  CHECK(!graphql::fleetSessionLimitOf(out.errors));
  // Busy with no wait named: present, and the wait absent (never a zero).
  out = open(refusal("PLATFORM_BUSY", R"(,"blame":"PLATFORM")"));
  CHECK(graphql::platformBusyOf(out.errors).has_value());
  CHECK(!graphql::platformBusyOf(out.errors)->retryAfterMs.has_value());

  // What the managed-player calls already report.
  out = open(refusal("ORG_LEGAL_ACCEPTANCE_REQUIRED"));
  CHECK(graphql::orgLegalAcceptanceRequiredOf(out.errors));
  out = open(refusal("FORBIDDEN"));
  CHECK(graphql::errorWithCode(out.errors, "FORBIDDEN") != nullptr);
  CHECK(!graphql::orgTokenRequiredOf(out.errors));
  out = open(refusal("FEATURE_DISABLED"));
  CHECK(graphql::errorWithCode(out.errors, "FEATURE_DISABLED") != nullptr);

  // refresh: the session expired, was closed, or was opened with another org token.
  rig.http->reply = refusal("NOT_FOUND", R"(,"httpStatus":404)");
  bool called = false;
  api.refreshAsync("7", "9001", "10.0.0.5", 7777,
                   [&](graphql::GraphQLOutcome refused, FleetSessionResponse answer) {
                     called = true;
                     CHECK(!refused.ok());
                     CHECK(graphql::fleetSessionNotFoundOf(refused.errors));
                     CHECK(!graphql::orgTokenRequiredOf(refused.errors));
                     CHECK(answer.token.empty());
                   });
  CHECK(called);
  // and busy, on a refresh, with its wait.
  rig.http->reply = refusal("PLATFORM_BUSY", R"(,"retryAfterMs":250)");
  called = false;
  api.refreshAsync("7", "9001", [&](graphql::GraphQLOutcome refused, FleetSessionResponse) {
    called = true;
    CHECK(graphql::platformBusyOf(refused.errors)->retryAfterMs == std::optional<std::int64_t>(250));
  });
  CHECK(called);

#ifndef CROWDY_NO_EXCEPTIONS
  // The blocking calls throw the same refusal.
  const auto thrown = [&](const std::string& reply, auto call) {
    rig.http->reply = reply;
    std::vector<graphql::GraphQLErrorDetail> errors;
    bool threw = false;
    try {
      call();
    } catch (const graphql::CrowdyGraphQLError& e) {
      threw = true;
      errors = e.errors();
    }
    CHECK(threw);
    return errors;
  };
  CHECK(graphql::orgTokenRequiredOf(thrown(refusal("ORG_TOKEN_REQUIRED"), [&] { api.open("7"); })));
  CHECK(graphql::fleetSessionLimitOf(thrown(refusal("FLEET_SESSION_LIMIT"), [&] { api.open("7"); })));
  CHECK(graphql::fleetSessionNotFoundOf(thrown(refusal("NOT_FOUND"), [&] { api.refresh("7", "9001"); })));
  const auto busyThrown = graphql::platformBusyOf(
      thrown(refusal("PLATFORM_BUSY", R"(,"retryAfterMs":900)"), [&] { api.close("7", "9001"); }));
  CHECK(busyThrown.has_value());
  CHECK(busyThrown->retryAfterMs == std::optional<std::int64_t>(900));
#endif
}

// An answer becomes the wire class's configuration, and a refresh answer reaches a session.
void testAnswersReachTheWireClass() {
  Rig rig;
  FleetSessionsAPI api(rig.gql);
  rig.http->reply = sessionReply("openFleetSession", kExpiresAt);
  const FleetSessionResponse opened = api.open("7");

  const replication::TokenInfo token = domains::fleetTokenOf(opened);
  CHECK_EQ(token.token, kFleetToken);
  CHECK_EQ(token.gameTokenId, 9001);
  CHECK_EQ(token.expiresAtEpochMs, kExpiresAtMs);
  CHECK(kExpiresAtMs - kOpenedAtMs == 24LL * 60 * 60 * 1000);
  const replication::Assignment server = domains::fleetServerOf(opened);
  CHECK_EQ(server.ip4, std::string("10.0.0.5"));
  CHECK_EQ(server.clientPort, 7777);

  // The caller's own options stay; the app, the token and the server come from the answer.
  replication::FleetConfig base;
  base.appId = 99;
  base.carryResendMs = 100;
  base.fleetOwnState = true;
  base.transport.manualPump = true;
  const auto config = domains::fleetConfigOf(opened, base);
  CHECK(config.ok());
  CHECK_EQ(config.value().appId, 7);
  CHECK_EQ(config.value().token.token, kFleetToken);
  CHECK_EQ(config.value().token.gameTokenId, 9001);
  CHECK_EQ(config.value().token.expiresAtEpochMs, kExpiresAtMs);
  CHECK_EQ(config.value().server.ip4, std::string("10.0.0.5"));
  CHECK_EQ(config.value().server.clientPort, 7777);
  CHECK_EQ(config.value().carryResendMs, 100);
  CHECK(config.value().fleetOwnState);
  CHECK(config.value().transport.manualPump);
  // The carry burst is raised to the session's maxPlayers (256 here; the server allows a
  // burst of maxPlayers + 32), and a larger one the caller chose is kept.
  CHECK_EQ(base.carryBurst, 32);
  CHECK_EQ(config.value().carryBurst, 256);
  CHECK_EQ(config.value().carriesPerSecond, 50);
  base.carryBurst = 1000;
  CHECK_EQ(domains::fleetConfigOf(opened, base).value().carryBurst, 1000);
  base.carryBurst = 32;
  CHECK(domains::fleetConfigOf(opened).ok());

  // An answer that cannot be a session's is refused.
  FleetSessionResponse bad = opened;
  bad.token = "short";
  CHECK(domains::fleetConfigOf(bad).error() == Errc::InvalidArgument);
  bad = opened;
  bad.gameTokenId = "nine";
  CHECK(domains::fleetConfigOf(bad).error() == Errc::InvalidArgument);
  bad = opened;
  bad.appId.clear();
  CHECK(domains::fleetConfigOf(bad).error() == Errc::InvalidArgument);
  bad = opened;
  bad.server.ip4.clear();
  CHECK(domains::fleetConfigOf(bad).error() == Errc::InvalidArgument);
  bad = opened;
  bad.server.clientPort = 0;
  CHECK(domains::fleetConfigOf(bad).error() == Errc::InvalidArgument);

  // A session built from it, on a clock at the moment it was opened. It is never connected.
  ManualClock clock;
  clock.epoch = kOpenedAtMs;
  replication::FleetSession fleet(config.value(), core::defaultCrypto(), clock);
  CHECK_EQ(fleet.appId(), 7);
  CHECK_EQ(fleet.token().gameTokenId, 9001);
  CHECK_EQ(fleet.server().clientPort, 7777);
  CHECK(!fleet.moveOrdered());

  // refreshDue: once half of the token's 24 hours is gone.
  CHECK(!fleet.refreshDue());
  clock.advance(12LL * 60 * 60 * 1000 - 1);
  CHECK(!fleet.refreshDue());
  clock.advance(1);
  CHECK(fleet.refreshDue());

  // refresh(session) names the session and the server it is on.
  const std::string laterExpiry = "2026-11-27T12:00:00.000Z";
  const std::int64_t laterExpiryMs = core::parseIso8601Millis(laterExpiry.data(), laterExpiry.size());
  rig.http->reply = sessionReply("refreshFleetSession", laterExpiry);
  const FleetSessionResponse refreshed = api.refresh(fleet);
  CHECK_EQ(bodyOf(rig.http->requests.back())["variables"].dump(),
           std::string(R"({"input":{"appId":"7","currentServer":{"clientPort":7777,"ip4":"10.0.0.5"},"gameTokenId":"9001"}})"));
  // The same server came back: the new expiry is taken and nothing moves.
  CHECK(domains::applyFleetRefresh(fleet, refreshed).ok());
  CHECK_EQ(fleet.token().expiresAtEpochMs, laterExpiryMs);
  CHECK_EQ(fleet.token().token, kFleetToken);
  CHECK_EQ(fleet.server().clientPort, 7777);
  CHECK(!fleet.refreshDue());  // 24 hours from this refresh
  clock.advance(12LL * 60 * 60 * 1000);
  CHECK(fleet.refreshDue());

  // Another server came back: the session is given it.
  bool called = false;
  rig.http->reply = sessionReply("refreshFleetSession", laterExpiry, "10.0.0.9", 7001);
  api.refreshAsync(fleet, [&](graphql::GraphQLOutcome out, FleetSessionResponse answer) {
    called = true;
    CHECK(out.ok());
    CHECK(domains::applyFleetRefresh(fleet, answer).ok());
  });
  CHECK(called);
  CHECK_EQ(fleet.server().ip4, std::string("10.0.0.9"));
  CHECK_EQ(fleet.server().clientPort, 7001);

  // Another session's answer, and a failed call's empty one, change nothing.
  FleetSessionResponse other = refreshed;
  other.gameTokenId = "9002";
  CHECK(domains::applyFleetRefresh(fleet, other).code == Errc::InvalidArgument);
  other = refreshed;
  other.appId = "8";
  CHECK(domains::applyFleetRefresh(fleet, other).code == Errc::InvalidArgument);
  CHECK(domains::applyFleetRefresh(fleet, FleetSessionResponse{}).code == Errc::InvalidArgument);
  other = refreshed;
  other.server.clientPort = 0;
  other.expiresAt = "2026-12-01T00:00:00.000Z";
  CHECK(domains::applyFleetRefresh(fleet, other).code == Errc::InvalidArgument);
  CHECK_EQ(fleet.token().expiresAtEpochMs, laterExpiryMs);
  CHECK_EQ(fleet.server().clientPort, 7001);
  CHECK_EQ(fleet.token().gameTokenId, 9001);

  // close(session) names it the same way.
  rig.http->reply = R"({"data":{"closeFleetSession":true}})";
  CHECK(api.close(fleet));
  CHECK_EQ(bodyOf(rig.http->requests.back())["variables"].dump(),
           std::string(R"({"input":{"appId":"7","gameTokenId":"9001"}})"));
  called = false;
  api.closeAsync(fleet, [&](graphql::GraphQLOutcome out, bool closed) {
    called = true;
    CHECK(out.ok() && closed);
  });
  CHECK(called);

  // A token with no expiry is never due.
  replication::FleetConfig forever = config.value();
  forever.token.expiresAtEpochMs = 0;
  replication::FleetSession unending(forever, core::defaultCrypto(), clock);
  clock.advance(365LL * 24 * 60 * 60 * 1000);
  CHECK(!unending.refreshDue());
  // The wire class takes a refresh without the API too, and checks both parts first.
  CHECK(unending.applyRefresh(replication::TokenInfo{kFleetToken, 9001, 5}, replication::Assignment{"", "", 1}).code ==
        Errc::InvalidArgument);
  CHECK(unending.applyRefresh(replication::TokenInfo{"short", 9001, 5}, replication::Assignment{"10.0.0.1", "", 1}).code ==
        Errc::InvalidArgument);
  CHECK_EQ(unending.token().expiresAtEpochMs, 0);
  CHECK(unending.applyRefresh(replication::TokenInfo{kFleetToken, 9001, clock.epochMillis() + 1000},
                              replication::Assignment{"10.0.0.1", "", 1})
            .ok());
  CHECK_EQ(unending.server().ip4, std::string("10.0.0.1"));
  CHECK(!unending.refreshDue());
  clock.advance(500);
  CHECK(unending.refreshDue());
}

#ifdef CROWDY_TEST_FLEET_LOOPBACK
struct UdpServer {
  int fd = -1;
  int port = 0;
  sockaddr_in client{};
  socklen_t clientLen = 0;
  UdpServer() {
    fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
  }
  ~UdpServer() { ::close(fd); }
  std::vector<std::uint8_t> recvOne() {
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    clientLen = sizeof(client);
    const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&client), &clientLen);
    CHECK(n > 0);
    return std::vector<std::uint8_t>(buf, buf + n);
  }
  /// The next datagram of `type`, skipping any other.
  std::vector<std::uint8_t> next(std::uint8_t type) {
    for (int i = 0; i < 16; ++i) {
      auto got = recvOne();
      if (got[0] == type) return got;
    }
    CHECK(false);
    return {};
  }
  void send(const std::vector<std::uint8_t>& data) {
    CHECK(::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&client), clientLen) ==
          static_cast<ssize_t>(data.size()));
  }
};

// A realtime server tells the session to move and nobody answers inside the session: it says
// so, a refresh then names no server, and the answer's server is where it goes.
void testAMoveOrderAsksForANewServer() {
  UdpServer first, second;
  Rig rig;
  FleetSessionsAPI api(rig.gql);
  rig.http->reply = sessionReply("openFleetSession", kExpiresAt, "127.0.0.1", first.port);
  replication::FleetConfig base;
  base.transport.manualPump = true;
  base.transport.sessionReadyWaitMs = 0;
  base.transport.bundleSends = false;
  ManualClock clock;
  clock.epoch = kOpenedAtMs;
  replication::FleetSession fleet(domains::fleetConfigOf(api.open("7"), base).value(), core::opensslCrypto(),
                                  clock);
  CHECK(fleet.open().ok());
  fleet.pump(0);
  CHECK(fleet.state() == replication::ConnState::Connected);
  CHECK(!fleet.moveOrdered());
  CHECK(!fleet.refreshDue());

  // The session's capabilities are the first thing the server hears; then its order to move
  // (COMMAND_RECONNECT, signed with the fleet token).
  CHECK_EQ(first.recvOne()[0], 29u);
  CHECK(fleet.sendHeartbeat().ok());
  CHECK_EQ(first.recvOne()[0], 26u);
  std::vector<std::uint8_t> reconnect(wire::kCommandReconnectSize);
  reconnect[0] = 22;
  CHECK(core::opensslCrypto().hmacSha256(asBytes(kFleetToken), Bytes(reconnect.data(), 1), reconnect.data() + 1));
  first.send(reconnect);
  for (int i = 0; i < 100 && !fleet.moveOrdered(); ++i) {
    fleet.pump(20);
    fleet.poll();
  }
  CHECK(fleet.moveOrdered());
  CHECK(fleet.refreshDue());  // at once, with most of the token's lifetime left
  CHECK_EQ(fleet.server().clientPort, first.port);

  // The refresh names no current server, and the answer's server is the next one.
  rig.http->reply = sessionReply("refreshFleetSession", kExpiresAt, "127.0.0.1", second.port);
  const FleetSessionResponse refreshed = api.refresh(fleet);
  CHECK_EQ(bodyOf(rig.http->requests.back())["variables"].dump(),
           std::string(R"({"input":{"appId":"7","gameTokenId":"9001"}})"));
  CHECK(domains::applyFleetRefresh(fleet, refreshed).ok());
  CHECK(!fleet.moveOrdered());
  CHECK(!fleet.refreshDue());
  CHECK_EQ(fleet.server().clientPort, second.port);
  for (int i = 0; i < 100 && !(fleet.sessionEpoch() >= 3 && fleet.state() == replication::ConnState::Connected); ++i) {
    fleet.pump(20);
    fleet.poll();
  }
  CHECK(fleet.state() == replication::ConnState::Connected);
  CHECK_EQ(second.recvOne()[0], 29u);  // its capabilities, on the new server
  CHECK(fleet.sendHeartbeat().ok());
  CHECK_EQ(second.recvOne()[0], 26u);

  // Connected again, a refresh names the server it is on.
  rig.http->reply = sessionReply("refreshFleetSession", kExpiresAt, "127.0.0.1", second.port);
  api.refresh(fleet);
  CHECK_EQ(bodyOf(rig.http->requests.back())["variables"]["input"]["currentServer"]["clientPort"].asInt64(),
           second.port);
  CHECK_EQ(bodyOf(rig.http->requests.back())["variables"]["input"]["currentServer"]["ip4"].asString(),
           std::string("127.0.0.1"));

  // With FleetConfig::reassign the session asks the game server at once, and no order is left.
  int asked = 0;
  replication::FleetConfig answering = domains::fleetConfigOf(refreshed, base).value();
  answering.reassign = [&]() -> Result<replication::Assignment> {
    ++asked;
    return replication::Assignment{"127.0.0.1", "", first.port};
  };
  replication::FleetSession answered(answering, core::opensslCrypto(), clock);
  CHECK(answered.open().ok());
  answered.pump(0);
  CHECK(answered.sendHeartbeat().ok());
  CHECK_EQ(second.next(26)[0], 26u);
  second.send(reconnect);
  for (int i = 0; i < 100 && asked == 0; ++i) {
    answered.pump(20);
    answered.poll();
  }
  CHECK_EQ(asked, 1);
  CHECK(!answered.moveOrdered());
  CHECK(!answered.refreshDue());
  CHECK_EQ(answered.server().clientPort, first.port);
}
#endif

}  // namespace

int main() {
  testOpenSendsTheAppAndReadsTheAnswer();
  testRefreshNamesTheSessionAndItsServer();
  testCloseAnswersWhetherASessionEnded();
  testRefusalsAreToldApart();
  testAnswersReachTheWireClass();
#ifdef CROWDY_TEST_FLEET_LOOPBACK
  testAMoveOrderAsksForANewServer();
#endif
  std::printf("fleet_sessions_api_test passed\n");
  return 0;
}
