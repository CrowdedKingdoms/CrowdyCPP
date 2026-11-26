// Live evidence for an organization's game servers: two fleet sessions that carry four of
// the organization's players between them, over native UDP, and the hub connection a game
// server opens for itself.
//
// Required (the suite exits 77 without them):
//   CROWDY_FLEET_API_URL     the Application API
//   CROWDY_FLEET_APP_ID      an app of the organization that grants its players text and voice
//   CROWDY_FLEET_ORG_TOKEN   an org token of that organization with the scopes fleet:session
//                            and players:mint. Read from the environment, never printed.
// Optional:
//   CROWDY_FLEET_IDLE_SECONDS  the app's idle timeout when it is short (60, say): the suite
//                              then lets one carried player go quiet and requires its
//                              FLEET_RELEASED, reason 5. Unset, that step is skipped.
//   CROWDY_FLEET_HUB_APP_ID, CROWDY_FLEET_HUB_ORG_TOKEN, CROWDY_FLEET_HUB_PLAYER_ID
//                            a ck-exec app running the platform's probe (node types `bare`
//                            and `probe`), an org token of ITS organization with the scope
//                            fleet:session, and the user id of one of its players: the hub
//                            step. It needs the curl WebSocket transport (libcurl 8.13 or
//                            newer). Unset, or without the transport, the step is skipped.
//
// What it leaves on the tier: four managed players of the organization, external ids
// crowdycpp-fleet-e2e-0 to -3, which a rerun signs in again. It deletes nobody and changes
// no setting. Every fleet session it opens is closed through the API before it passes.
//
// The players stand in chunk (7100, 7100, 0). Each send is retried a few times where UDP may
// have dropped it; a refusal or a wrong delivery fails the run.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crowdy/domains/fleet_sessions.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/websocket.hpp"
#include "crowdy/replication/fleet_session.hpp"
#include "e2e_util.hpp"

using namespace e2e;
using namespace crowdy;
using namespace crowdy::replication;
using crowdy::domains::FleetSessionResponse;

namespace {

using Clock = std::chrono::steady_clock;

const wire::ChunkCoord kChunk{7100, 7100, 0};

// ---- nothing secret is printed ---------------------------------------------------------

/// Every token the run holds. Text on its way to the terminal passes through scrub().
std::vector<std::string>& secrets() {
  static std::vector<std::string> held;
  return held;
}

void keepSecret(const std::string& value) {
  if (!value.empty()) secrets().push_back(value);
}

/// `text` without any token the run holds, and without anything shaped like an org token.
std::string scrub(std::string text) {
  for (const auto& secret : secrets()) {
    for (std::size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at)) {
      text.replace(at, secret.size(), "[redacted]");
    }
  }
  for (std::size_t at = text.find("cko_"); at != std::string::npos; at = text.find("cko_", at)) {
    std::size_t end = at + 4;
    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) ||
                                 text[end] == '_' || text[end] == '-')) {
      ++end;
    }
    text.replace(at, end - at, "[redacted]");
    at += 10;
  }
  return text;
}

void say(const std::string& line) { std::printf("%s\n", scrub(line).c_str()); }

[[noreturn]] void fail(const std::string& why) {
  std::fprintf(stderr, "e2e_fleet_session FAILED: %s\n", scrub(why).c_str());
  std::exit(1);
}

#define FLEET_CHECK(cond, why)                                                         \
  do {                                                                                 \
    if (!(cond)) fail(std::string(why) + " (" #cond ", line " + std::to_string(__LINE__) + ")"); \
  } while (0)

std::int64_t msSince(Clock::time_point from) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count();
}

std::string str(Bytes bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Bytes bytesOf(const std::string& text) {
  return Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

// ---- the Application API ---------------------------------------------------------------

/// Counts the HTTP requests a client makes, so a step can show it made none.
class CountingTransport final : public graphql::IHttpTransport {
 public:
  explicit CountingTransport(std::shared_ptr<graphql::IHttpTransport> inner)
      : inner_(std::move(inner)) {}

  graphql::HttpResponse send(const graphql::HttpRequest& request) override {
    ++count_;
    return inner_->send(request);
  }
  graphql::HttpOutcome sendOutcome(const graphql::HttpRequest& request) noexcept override {
    ++count_;
    return inner_->sendOutcome(request);
  }
  int count() const { return count_.load(); }

 private:
  std::shared_ptr<graphql::IHttpTransport> inner_;
  std::atomic<int> count_{0};
};

/// An API call, repeated when the platform says it is busy (PLATFORM_BUSY names the wait).
template <typename Fn>
auto whenNotBusy(Fn&& call) -> decltype(call()) {
  for (int attempt = 0;; ++attempt) {
    try {
      return call();
    } catch (const graphql::CrowdyGraphQLError& e) {
      const auto busy = graphql::platformBusyOf(e.errors());
      if (!busy || attempt >= 5) throw;
      const std::int64_t wait = std::clamp<std::int64_t>(busy->retryAfterMs.value_or(1000), 200, 5000);
      say("the platform is busy; waiting " + std::to_string(wait) + " ms");
      std::this_thread::sleep_for(std::chrono::milliseconds(wait));
    }
  }
}

// ---- what a session received -----------------------------------------------------------

struct Update {
  core::ActorUuid uuid;
  std::string payload;
  bool forSession = false;
};

struct Delivery {
  std::string payload;
  bool forSession = false;
  std::vector<std::int64_t> players;
};

struct ErrorSeen {
  std::uint8_t sequence = 0;
  wire::ErrorCode code = wire::ErrorCode::NoError;
  bool verified = false;
  Clock::time_point at;
};

struct ReleasedSeen {
  FleetReleased released;
  Clock::time_point at;
};

struct StatusSeen {
  ConnState state;
  Clock::time_point at;
};

/// One of the organization's game servers: its session at the API and on the wire.
struct GameServer {
  explicit GameServer(std::string called) : name(std::move(called)) {}

  std::string name;
  FleetSessionResponse opened;
  std::unique_ptr<FleetSession> fleet;

  std::vector<Update> updates;
  std::vector<Delivery> texts;
  std::vector<ErrorSeen> errors;
  std::vector<ReleasedSeen> released;
  std::vector<StatusSeen> statuses;
  std::vector<FleetCarryResult> results;
  /// Communications that arrived outside a FLEET_DELIVERY: there must be none.
  int plainCommunications = 0;
  Clock::time_point lastOwnHeartbeat{};

  void connect(std::int64_t watchdogSilenceMs = 0) {
    FleetConfig base;
    base.transport.watchdogSilenceMs = watchdogSilenceMs;
    auto config = domains::fleetConfigOf(opened, base);
    FLEET_CHECK(config.ok(), name + ": the open answer makes a FleetConfig");
    fleet = std::make_unique<FleetSession>(config.value(), core::opensslCrypto());
    FleetHandlers h;
    h.actorUpdate = [this](const SpatialNotification& n, FleetRecipients r) {
      updates.push_back({n.uuidArray(), str(n.payload), r.forSession()});
    };
    h.text = [this](const SpatialNotification& n, FleetRecipients r) {
      texts.push_back({str(n.payload), r.forSession(), std::vector<std::int64_t>(r.begin(), r.end())});
      if (r.forSession()) ++plainCommunications;
    };
    const auto communication = [this](const SpatialNotification&, FleetRecipients r) {
      if (r.forSession()) ++plainCommunications;
    };
    h.audio = communication;
    h.video = communication;
    h.singleActorMessage = communication;
    const auto channel = [this](const ChannelNotification&, FleetRecipients r) {
      if (r.forSession()) ++plainCommunications;
    };
    h.channelMessage = channel;
    h.channelAudio = channel;
    h.genericError = [this](const GenericError& e) {
      errors.push_back({e.sequence, e.code, e.verified, Clock::now()});
    };
    h.released = [this](const FleetReleased& r) { released.push_back({r, Clock::now()}); };
    h.status = [this](ConnState s) { statuses.push_back({s, Clock::now()}); };
    h.carryResult = [this](const FleetCarryResult& r) { results.push_back(r); };
    fleet->setHandlers(std::move(h));
    FLEET_CHECK(fleet->open().ok(), name + ": the session opens");
  }

  int updatesFrom(const core::ActorUuid& uuid) const {
    return static_cast<int>(std::count_if(updates.begin(), updates.end(),
                                           [&](const Update& u) { return u.uuid == uuid; }));
  }
  int updatesWith(const std::string& payload) const {
    return static_cast<int>(std::count_if(updates.begin(), updates.end(),
                                           [&](const Update& u) { return u.payload == payload; }));
  }
  std::vector<const Delivery*> textsWith(const std::string& payload) const {
    std::vector<const Delivery*> found;
    for (const auto& t : texts) {
      if (t.payload == payload) found.push_back(&t);
    }
    return found;
  }
  const ReleasedSeen* releasedOf(std::int64_t player, std::size_t from = 0) const {
    for (std::size_t i = from; i < released.size(); ++i) {
      if (released[i].released.playerGameTokenId == player) return &released[i];
    }
    return nullptr;
  }
};

struct Managed {
  std::string externalId;
  PlayerToken token;
  std::int64_t userId = 0;
  core::ActorUuid uuid{};
  GameServer* carrier = nullptr;  ///< the server that sends for the player now
  bool quiet = false;             ///< no keepalive: the player is being let go idle
  Clock::time_point lastInput{};
};

/// The run: two servers, two more for the ended-session step, four players.
struct World {
  std::vector<GameServer*> servers;
  std::vector<Managed> players;
  Clock::time_point lastKeepalive{};
  std::string tag;
  int keepalives = 0;

  /// Dispatch what arrived, and keep every carried player present: an actor update a second
  /// for each (the app's idle timeout may be as short as a minute), and each session's own
  /// keepalive every five.
  void tick() {
    const auto now = Clock::now();
    if (now - lastKeepalive >= std::chrono::seconds(1)) {
      lastKeepalive = now;
      for (auto& p : players) {
        if (!p.carrier || p.quiet || !p.carrier->fleet) continue;
        if (!p.carrier->fleet->carries(p.token.gameTokenId)) continue;
        // A new pose each time: the same bytes again need not count as input.
        const std::string pose = "ka " + tag + " " + std::to_string(++keepalives);
        if (p.carrier->fleet->sendActorUpdate(p.token.gameTokenId, {kChunk, p.uuid, bytesOf(pose)}).ok())
          p.lastInput = now;
      }
      for (auto* s : servers) {
        if (!s->fleet) continue;
        if (now - s->lastOwnHeartbeat >= std::chrono::seconds(5)) {
          s->lastOwnHeartbeat = now;
          (void)s->fleet->sendHeartbeat();
        }
        (void)s->fleet->flushSends();
      }
    }
    for (auto* s : servers) {
      if (s->fleet) s->fleet->poll();
    }
  }

  template <typename Done>
  bool until(Done&& done, int timeoutMs = 8000) {
    const auto start = Clock::now();
    for (;;) {
      tick();
      if (done()) return true;
      if (msSince(start) > timeoutMs) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void spin(int ms) {
    (void)until([] { return false; }, ms);
  }

  /// `attempt(n)` sends something new each time; `done()` says it arrived. UDP may drop one.
  template <typename Attempt, typename Done>
  bool retry(Attempt&& attempt, Done&& done, int attempts = 5, int perWaitMs = 3000) {
    for (int n = 0; n < attempts; ++n) {
      attempt(n);
      if (until(done, perWaitMs)) return true;
    }
    return false;
  }
};

std::string names(const std::vector<std::int64_t>& listed, const World& w) {
  std::string out;
  for (std::int64_t id : listed) {
    std::string who = std::to_string(id);
    for (std::size_t i = 0; i < w.players.size(); ++i) {
      if (w.players[i].token.gameTokenId == id) who = "player " + std::to_string(i);
    }
    out += (out.empty() ? "" : ", ") + who;
  }
  return out.empty() ? "nobody" : out;
}

bool sameSet(std::vector<std::int64_t> a, std::vector<std::int64_t> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

const char* stateName(ConnState s) {
  switch (s) {
    case ConnState::Idle: return "Idle";
    case ConnState::Connecting: return "Connecting";
    case ConnState::Connected: return "Connected";
    case ConnState::Reconnecting: return "Reconnecting";
    case ConnState::Failed: return "Failed";
    case ConnState::Closed: return "Closed";
    case ConnState::Inactive: return "Inactive";
  }
  return "?";
}

/// One carry, timed from the call to its result.
struct CarryOutcome {
  FleetCarryResult result;
  std::int64_t ms = -1;
};

CarryOutcome carry(World& w, GameServer& server, Managed& player) {
  CarryOutcome out;
  bool done = false;
  const auto start = Clock::now();
  FLEET_CHECK(server.fleet
                  ->carry(player.token,
                          [&](const FleetCarryResult& r) {
                            out.result = r;
                            out.ms = msSince(start);
                            done = true;
                          })
                  .ok(),
              server.name + ": carry() is accepted");
  FLEET_CHECK(w.until([&] { return done; }, 12000), server.name + ": the carry has a result");
  return out;
}

// ---- the hub connection ------------------------------------------------------------------

/// Step 10. Returns why it was skipped, or empty when it ran.
std::string hubStep(const std::string& apiUrl) {
  const std::string appId = envOr("CROWDY_FLEET_HUB_APP_ID");
  const std::string orgToken = envOr("CROWDY_FLEET_HUB_ORG_TOKEN");
  const std::string playerText = envOr("CROWDY_FLEET_HUB_PLAYER_ID");
  if (appId.empty() || orgToken.empty() || playerText.empty())
    return "CROWDY_FLEET_HUB_APP_ID, _ORG_TOKEN and _PLAYER_ID are not all set";
  keepSecret(orgToken);
  if (!graphql::makeCurlWebSocketTransport())
    return "this build has no WebSocket transport (libcurl older than 8.13)";
  const std::uint64_t player = std::strtoull(playerText.c_str(), nullptr, 10);
  FLEET_CHECK(player != 0, "CROWDY_FLEET_HUB_PLAYER_ID is a user id");

  ClientConfig c;
  c.httpUrl = apiUrl;
  CrowdyClient server(std::move(c));
  server.setToken(orgToken);

  auto hub = server.exec().connectAsServer(appId);
  FLEET_CHECK(hub && hub->isServer(), "connectAsServer makes a server connection");

  const auto call = [&](const char* what, const std::function<void(domains::ExecReplyCallback)>& make) {
    std::optional<domains::ExecReply> reply;
    make([&](domains::ExecReply r) { reply = std::move(r); });
    const auto start = Clock::now();
    while (!reply && msSince(start) < 20000) {
      server.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FLEET_CHECK(reply.has_value(), std::string(what) + " is answered");
    if (!reply->ok()) fail(std::string(what) + " was refused: " + reply->message());
    const std::string value = reply->value().dump();
    say(std::string("   ") + what + " -> " + value + " (" + std::to_string(msSince(start)) + " ms)");
    return value;
  };

  const std::string own = call("the server's own call, bare/k whoami", [&](domains::ExecReplyCallback done) {
    hub->call("bare", "k", "whoami", graphql::JVal(), std::move(done));
  });
  FLEET_CHECK(own.find("Org { player: None }") != std::string::npos,
              "a hub sees the server's own call as Org { player: None }");

  const std::string as = call("a call for the player, bare/k whoami", [&](domains::ExecReplyCallback done) {
    hub->callAs(player, "bare", "k", "whoami", graphql::JVal(), std::move(done));
  });
  FLEET_CHECK(as.find("Org { player: Some(" + playerText + ") }") != std::string::npos,
              "a hub sees the call-as as Org { player: Some(id) }");

  const std::string named = call("the probe's server endpoint for the player", [&](domains::ExecReplyCallback done) {
    hub->callAs(player, "probe", "", "server", graphql::JVal(), std::move(done));
  });
  FLEET_CHECK(named == playerText || named == "\"" + playerText + "\"",
              "the probe's server endpoint answers the player the server named");

  hub->close();
  return {};
}

// ---- the run -----------------------------------------------------------------------------

int run() {
  const std::string apiUrl = envOr("CROWDY_FLEET_API_URL");
  const std::string appId = envOr("CROWDY_FLEET_APP_ID");
  const std::string orgToken = envOr("CROWDY_FLEET_ORG_TOKEN");
  if (apiUrl.empty() || appId.empty() || orgToken.empty()) {
    std::puts("CROWDY_FLEET_API_URL / _APP_ID / _ORG_TOKEN not configured; skipping");
    return 77;
  }
  keepSecret(orgToken);
  const int idleSeconds = std::atoi(envOr("CROWDY_FLEET_IDLE_SECONDS", "0").c_str());

  auto http = std::make_shared<CountingTransport>(graphql::makeCurlTransport());
  ClientConfig c;
  c.httpUrl = apiUrl;
  c.transport = http;
  CrowdyClient api(std::move(c));
  api.setToken(orgToken);
  auto& sessions = api.admin().fleetSessions();

  World w;
  w.tag = runSuffix();
  GameServer X("X"), Y("Y"), Z("Z"), V("V");
  w.servers = {&X, &Y, &Z, &V};

  // ---- 1 ----
  E2E_SUBTEST("1. two game servers open a fleet session each");
  const auto wellFormed = [&](GameServer& s) {
    keepSecret(s.opened.token);
    FLEET_CHECK(s.opened.token.size() == 64, s.name + ": the token is 64 characters");
    FLEET_CHECK(s.opened.gameTokenIdInt64().value_or(0) > 0, s.name + ": it has a gameTokenId");
    FLEET_CHECK(s.opened.appId == appId, s.name + ": it is the app's");
    FLEET_CHECK(s.opened.maxPlayers >= 4, s.name + ": it may carry the four players");
    FLEET_CHECK(!s.opened.server.ip4.empty() && s.opened.server.clientPort > 0,
                s.name + ": the answer names a realtime server");
    const std::int64_t expires =
        core::parseIso8601Millis(s.opened.expiresAt.data(), s.opened.expiresAt.size());
    const std::int64_t hours = (expires - core::systemClock().epochMillis()) / 3'600'000;
    FLEET_CHECK(hours >= 23 && hours <= 24, s.name + ": it expires in 24 hours");
    say("   " + s.name + ": session " + s.opened.gameTokenId + ", may carry " +
        std::to_string(s.opened.maxPlayers) + ", realtime server id " + s.opened.server.serverId);
  };
  X.opened = whenNotBusy([&] { return sessions.open(appId); });
  wellFormed(X);
  Y.opened = whenNotBusy([&] { return sessions.open(appId); });
  wellFormed(Y);
  FLEET_CHECK(X.opened.gameTokenId != Y.opened.gameTokenId && X.opened.token != Y.opened.token,
              "the two sessions are two sessions");

  // ---- 2 ----
  E2E_SUBTEST("2. the organization signs four of its players in (mintManagedPlayerToken)");
  for (int i = 0; i < 4; ++i) {
    Managed p;
    p.externalId = "crowdycpp-fleet-e2e-" + std::to_string(i);
    graphql::JVal vars;
    auto& input = vars["input"];
    input["appId"] = appId;
    input["externalId"] = p.externalId;
    input["gamertag"] = p.externalId;
    input["ageYears"] = 30;
    input["country"] = "US";
    graphql::GraphQLOutcome out;
    for (int attempt = 0; attempt < 6; ++attempt) {
      // The mint is an organization's server call that this SDK does not wrap.
      out = api.graphqlClient().requestOutcome(
          "mutation Mint($input: MintManagedPlayerTokenInput!) {"
          " mintManagedPlayerToken(input: $input) { token gameTokenId appId expiresAt } }",
          vars);
      const auto busy = graphql::platformBusyOf(out.errors);
      if (out.ok() || !busy) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::clamp<std::int64_t>(busy->retryAfterMs.value_or(1000), 200, 5000)));
    }
    if (!out.ok()) {
      fail("signing " + p.externalId + " in was refused: " +
           (out.errors.empty() ? std::string(errcName(out.status.code))
                               : out.errors.front().code + " " + out.errors.front().message));
    }
    const auto minted = domains::AppTokenResponse::fromJson(out.data["mintManagedPlayerToken"]);
    keepSecret(minted.token);
    FLEET_CHECK(minted.token.size() == 64, p.externalId + ": the player's token is 64 characters");
    p.token = PlayerToken{minted.token, minted.gameTokenIdInt64().value_or(0)};
    FLEET_CHECK(p.token.gameTokenId > 0, p.externalId + ": it has a gameTokenId");
    p.uuid = core::generateActorUuid();
    say("   player " + std::to_string(i) + " (" + p.externalId + "): gameTokenId " +
        std::to_string(p.token.gameTokenId));
    w.players.push_back(std::move(p));
  }

  // ---- 3 ----
  E2E_SUBTEST("3. both sessions connect to their realtime server");
  for (GameServer* s : {&X, &Y}) {
    const auto start = Clock::now();
    s->connect();
    FLEET_CHECK(w.until([&] { return s->fleet->state() == ConnState::Connected; }, 15000),
                s->name + ": the session is connected");
    // Its capabilities bind it to its address; the server acknowledges them (46).
    const bool acked = w.until([&] { return s->fleet->stats().boundControlAcks > 0; }, 5000);
    say("   " + s->name + ": connected in " + std::to_string(msSince(start)) + " ms, capabilities " +
        (acked ? "acknowledged" : "NOT acknowledged"));
    FLEET_CHECK(acked, s->name + ": the server acknowledges the session's capabilities");
    FLEET_CHECK(!s->fleet->moveOrdered() && !s->fleet->refreshDue(), s->name + ": nothing is due");
  }

  // ---- 4 ----
  E2E_SUBTEST("4. X carries players 0 and 1, Y carries players 2 and 3");
  for (int i = 0; i < 4; ++i) {
    GameServer& server = i < 2 ? X : Y;
    Managed& p = w.players[i];
    const CarryOutcome out = carry(w, server, p);
    say("   " + server.name + " carries player " + std::to_string(i) + ": " +
        fleetCarryStatusName(out.result.status) + ", user " + std::to_string(out.result.userId) + ", " +
        std::to_string(out.ms) + " ms");
    FLEET_CHECK(out.result.carried(), "the carry ends Carried");
    FLEET_CHECK(out.result.userId > 0, "a carried player has a user id");
    FLEET_CHECK(server.fleet->carries(p.token.gameTokenId), "the session carries the player");
    p.userId = out.result.userId;
    p.carrier = &server;
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j)
      FLEET_CHECK(w.players[i].userId != w.players[j].userId, "four players are four users");
  }
  FLEET_CHECK(X.fleet->carriedCount() == 2 && Y.fleet->carriedCount() == 2, "two players each");
  Managed &p0 = w.players[0], &p1 = w.players[1], &p2 = w.players[2], &p3 = w.players[3];

  // ---- 5 ----
  E2E_SUBTEST("5. state: an actor update arrives once, plain, and a session is not sent its own players'");
  // Everyone stands in the chunk first (the keepalive's updates); the first messages of a
  // session in a chunk can be refused until its permissions there are loaded.
  FLEET_CHECK(w.until([&] { return Y.updatesFrom(p0.uuid) > 0 && Y.updatesFrom(p1.uuid) > 0 &&
                                   X.updatesFrom(p2.uuid) > 0 && X.updatesFrom(p3.uuid) > 0; },
                      30000),
              "each session receives the other session's players");
  const int kUpdates = 10;
  for (int n = 0; n < kUpdates; ++n) {
    const std::string pose = "upd " + w.tag + " " + std::to_string(n);
    FLEET_CHECK(X.fleet->sendActorUpdate(p0.token.gameTokenId, {kChunk, p0.uuid, bytesOf(pose)}).ok(),
                "X sends for player 0");
    (void)X.fleet->flushSends();
    w.spin(60);
  }
  w.until([&] {
    int got = 0;
    for (int n = 0; n < kUpdates; ++n) got += Y.updatesWith("upd " + w.tag + " " + std::to_string(n)) > 0;
    return got == kUpdates;
  }, 5000);
  int arrived = 0;
  for (int n = 0; n < kUpdates; ++n) {
    const int times = Y.updatesWith("upd " + w.tag + " " + std::to_string(n));
    FLEET_CHECK(times <= 1, "an update reaches the session once, however many of its players are in range");
    arrived += times;
  }
  say("   Y received " + std::to_string(arrived) + " of player 0's " + std::to_string(kUpdates) +
      " updates, each once");
  FLEET_CHECK(arrived >= kUpdates - 2, "player 0's updates reach Y");
  for (const auto& u : Y.updates) FLEET_CHECK(u.forSession, "state arrives plain, for the session");
  for (const auto& u : X.updates) FLEET_CHECK(u.forSession, "state arrives plain, for the session");
  FLEET_CHECK(X.updatesFrom(p0.uuid) == 0 && X.updatesFrom(p1.uuid) == 0,
              "X is not sent its own players' state");
  FLEET_CHECK(Y.updatesFrom(p2.uuid) == 0 && Y.updatesFrom(p3.uuid) == 0,
              "Y is not sent its own players' state");

  // ---- 6 ----
  E2E_SUBTEST("6. text: delivered only inside FLEET_DELIVERY, listing who may receive it");
  std::string text;
  FLEET_CHECK(w.retry(
                  [&](int n) {
                    text = "text " + w.tag + " " + std::to_string(n);
                    FLEET_CHECK(X.fleet->sendText(p0.token.gameTokenId, {kChunk, p0.uuid, bytesOf(text)}).ok(),
                                "X sends player 0's text");
                    (void)X.fleet->flushSends();
                  },
                  [&] { return !Y.textsWith(text).empty() && !X.textsWith(text).empty(); }),
              "player 0's text reaches both sessions");
  w.spin(500);  // anything more of it would have come by now
  {
    const auto onY = Y.textsWith(text), onX = X.textsWith(text);
    say("   Y: " + std::to_string(onY.size()) + " delivery listing " + names(onY[0]->players, w) + "; X: " +
        std::to_string(onX.size()) + " delivery listing " + names(onX[0]->players, w));
    FLEET_CHECK(onY.size() == 1 && onX.size() == 1, "one delivery a session");
    FLEET_CHECK(!onY[0]->forSession && !onX[0]->forSession, "a text never arrives plain");
    FLEET_CHECK(sameSet(onY[0]->players, {p2.token.gameTokenId, p3.token.gameTokenId}),
                "Y's delivery lists players 2 and 3");
    FLEET_CHECK(sameSet(onX[0]->players, {p0.token.gameTokenId, p1.token.gameTokenId}),
                "X's delivery lists players 0 and 1: a sender's own text lists the sender");
  }
  FLEET_CHECK(X.plainCommunications == 0 && Y.plainCommunications == 0,
              "no communication arrives outside a FLEET_DELIVERY");

  // ---- 7 ----
  E2E_SUBTEST("7. a crossing: Y carries player 0, with no Application API request");
  {
    const int requestsBefore = http->count();
    const std::size_t releasedBefore = X.released.size();
    const auto start = Clock::now();
    const CarryOutcome out = carry(w, Y, p0);
    FLEET_CHECK(out.result.carried(), "Y's carry of player 0 ends Carried");
    FLEET_CHECK(out.result.userId == p0.userId, "it is the same user");
    FLEET_CHECK(w.until([&] { return X.releasedOf(p0.token.gameTokenId, releasedBefore) != nullptr; }, 8000),
                "X is told player 0 is released");
    const ReleasedSeen* told = X.releasedOf(p0.token.gameTokenId, releasedBefore);
    const auto toldMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(told->at - start).count();
    say("   Y: Carried in " + std::to_string(out.ms) + " ms; X: released, reason " +
        fleetReleaseReasonName(told->released.reason) + " (" + std::to_string(told->released.wireReason) +
        "), " + std::to_string(toldMs) + " ms after Y's carry() was called");
    FLEET_CHECK(told->released.reason == FleetReleaseReason::CarriedElsewhere && told->released.wireReason == 1,
                "the reason is that another session carries the player");
    FLEET_CHECK(told->released.userId == p0.userId, "the release names the user");
    FLEET_CHECK(!X.fleet->carries(p0.token.gameTokenId), "X no longer carries player 0");
    const auto refused = X.fleet->sendActorUpdate(p0.token.gameTokenId, {kChunk, p0.uuid, bytesOf("no")});
    FLEET_CHECK(!refused.ok() && refused.error() == Errc::NotCarried, "a send for player 0 on X is NotCarried");
    FLEET_CHECK(Y.fleet->carries(p0.token.gameTokenId) && Y.fleet->carriedCount() == 3, "Y carries three");
    FLEET_CHECK(http->count() == requestsBefore, "the crossing made no Application API request");
    p0.carrier = &Y;
    // Player 0 is another session's player now, so X is sent its state.
    const std::string pose = "crossed " + w.tag;
    FLEET_CHECK(w.retry(
                    [&](int) {
                      (void)Y.fleet->sendActorUpdate(p0.token.gameTokenId, {kChunk, p0.uuid, bytesOf(pose)});
                      (void)Y.fleet->flushSends();
                    },
                    [&] { return X.updatesWith(pose) > 0; }),
                "after the crossing X receives player 0's state from Y");
  }

  // ---- 8 ----
  E2E_SUBTEST("8. a refusal: player 1 may not send under player 0's actor");
  {
    std::optional<ErrorSeen> refusal;
    std::string pose;
    FLEET_CHECK(w.retry(
                    [&](int n) {
                      pose = "not mine " + w.tag + " " + std::to_string(n);
                      const std::size_t from = X.errors.size();
                      const auto seq =
                          X.fleet->sendActorUpdate(p1.token.gameTokenId, {kChunk, p0.uuid, bytesOf(pose)});
                      FLEET_CHECK(seq.ok(), "the library sends it: the rule is the server's");
                      (void)X.fleet->flushSends();
                      w.until([&] {
                        for (std::size_t i = from; i < X.errors.size(); ++i) {
                          if (X.errors[i].sequence == seq.value()) refusal = X.errors[i];
                        }
                        return refusal.has_value();
                      }, 3000);
                    },
                    [&] { return refusal.has_value(); }, 5, 1),
                "the send is answered with an error frame");
    say(std::string("   answered code ") + std::to_string(static_cast<int>(refusal->code)) +
        (refusal->code == wire::ErrorCode::Unauthorized ? " (UNAUTHORIZED)" : "") + ", verified: " +
        (refusal->verified ? "true (inside a signed bundle whose HMAC held)" : "false (outside a signed bundle)"));
    FLEET_CHECK(refusal->code == wire::ErrorCode::Unauthorized, "the refusal is UNAUTHORIZED");
    w.spin(500);
    FLEET_CHECK(Y.updatesWith(pose) == 0, "the refused update reached nobody");
    FLEET_CHECK(X.fleet->carries(p1.token.gameTokenId) && X.fleet->state() == ConnState::Connected,
                "a refusal changes nothing in the session");
    std::printf("REFUSALS_VERIFIED=%s\n", refusal->verified ? "true" : "false");
  }

  // ---- 9 ----
  E2E_SUBTEST("9. refresh: the same token and id, a later expiry, the same server, the carries stand");
  {
    const TokenInfo before = X.fleet->token();
    const Assignment serverBefore = X.fleet->server();
    const auto carriesBefore = X.fleet->stats().messagesSentByType[49];
    const FleetSessionResponse answer = whenNotBusy([&] { return sessions.refresh(*X.fleet); });
    FLEET_CHECK(answer.token == before.token, "the refresh answers the same token");
    FLEET_CHECK(answer.gameTokenIdInt64().value_or(0) == before.gameTokenId, "and the same id");
    const std::int64_t expires = core::parseIso8601Millis(answer.expiresAt.data(), answer.expiresAt.size());
    FLEET_CHECK(expires > before.expiresAtEpochMs, "with a later expiry");
    FLEET_CHECK(answer.server.ip4 == serverBefore.ip4 && answer.server.clientPort == serverBefore.clientPort,
                "on the server the session is on");
    FLEET_CHECK(domains::applyFleetRefresh(*X.fleet, answer).ok(), "applyFleetRefresh takes it");
    say("   the expiry moved " + std::to_string((expires - before.expiresAtEpochMs) / 1000) + " s later");
    FLEET_CHECK(X.fleet->token().expiresAtEpochMs == expires, "the session holds the new expiry");
    FLEET_CHECK(X.fleet->carries(p1.token.gameTokenId), "the carry stands");
    w.spin(1500);
    FLEET_CHECK(X.fleet->state() == ConnState::Connected, "the session is still connected");
    FLEET_CHECK(X.fleet->server().ip4 == serverBefore.ip4, "to the same server");
    FLEET_CHECK(X.fleet->stats().messagesSentByType[49] == carriesBefore, "and it sent no FLEET_CARRY");
    FLEET_CHECK(!X.fleet->refreshDue() && !X.fleet->moveOrdered(), "nothing is due");
    const std::string pose = "after refresh " + w.tag;
    FLEET_CHECK(w.retry(
                    [&](int) {
                      (void)X.fleet->sendActorUpdate(p1.token.gameTokenId, {kChunk, p1.uuid, bytesOf(pose)});
                      (void)X.fleet->flushSends();
                    },
                    [&] { return Y.updatesWith(pose) > 0; }),
                "a send for the carried player still arrives on the other session");
    p1.lastInput = Clock::now();
  }

  // ---- 9b ----
  // Player 1 goes quiet from here: with CROWDY_FLEET_IDLE_SECONDS its idle release is step 9c.
  p1.quiet = true;
  const auto quietSince = Clock::now();
  const std::size_t xReleasedBefore = X.released.size();

  // A third server takes player 3 from Y, its session is closed through the API, and its
  // FleetSession goes on sending for player 3. What the realtime server then sends it is
  // printed; what is required is that the ended session no longer speaks for the player,
  // and, with the silence watchdog on, that the session learns it has to refresh.
  const auto endedSession = [&](GameServer& S, std::int64_t watchdogMs, int observeMs) {
    S.opened = whenNotBusy([&] { return sessions.open(appId); });
    wellFormed(S);
    S.connect(watchdogMs);
    FLEET_CHECK(w.until([&] { return S.fleet->state() == ConnState::Connected; }, 15000),
                S.name + " is connected");
    const std::size_t yReleasedBefore = Y.released.size();
    const CarryOutcome crossed = carry(w, S, p3);
    FLEET_CHECK(crossed.result.carried(), S.name + " carries player 3");
    FLEET_CHECK(w.until([&] { return Y.releasedOf(p3.token.gameTokenId, yReleasedBefore) != nullptr; }, 8000),
                "Y is told player 3 is released");
    say("   " + S.name + " carries player 3 in " + std::to_string(crossed.ms) + " ms (a crossing from Y" +
        (S.opened.server.serverId != Y.opened.server.serverId ? ", to another realtime server)" : ")"));
    p3.carrier = &S;
    const std::string live = "live " + S.name + " " + w.tag;
    FLEET_CHECK(w.retry(
                    [&](int) {
                      (void)S.fleet->sendActorUpdate(p3.token.gameTokenId, {kChunk, p3.uuid, bytesOf(live)});
                      (void)S.fleet->flushSends();
                    },
                    [&] { return Y.updatesWith(live) > 0; }),
                S.name + "'s sends for player 3 reach Y before the session is ended");
    w.spin(2000);            // some ordinary traffic first, to compare with
    p3.carrier = nullptr;    // from here this step sends for player 3 itself, numbered

    const auto statsBefore = S.fleet->stats();
    const std::size_t errorsBefore = S.errors.size(), statusesBefore = S.statuses.size(),
                      releasedBefore = S.released.size();
    FLEET_CHECK(sessions.close(*S.fleet), "closeFleetSession ends " + S.name + "'s session (true)");
    const auto closedAt = Clock::now();
    say("   before the close the realtime server had sent " + S.name + " " +
        std::to_string(statsBefore.datagramsReceived) + " datagrams (" +
        std::to_string(statsBefore.boundControlAcks) + " capability acknowledgements). Closed through the API; " +
        S.name + "'s FleetSession keeps sending for player 3, once a second" +
        (watchdogMs > 0 ? ", with transport.watchdogSilenceMs " + std::to_string(watchdogMs) : ""));

    std::map<std::string, int> sendResults;
    int arrivedCount = 0, arrivedLate = 0;
    std::int64_t lastArrivedSentMs = -1, moveOrderedAtMs = -1, refreshDueAtMs = -1;
    bool moveOrdered = false, refreshDue = false;
    ConnState state = S.fleet->state();
    const auto note = [&](const std::string& what) {
      say("   +" + std::to_string(msSince(closedAt)) + " ms: " + what);
    };
    for (int n = 0; msSince(closedAt) < observeMs; ++n) {
      const std::string pose = "after " + S.name + " " + w.tag + " " + std::to_string(n);
      const std::int64_t sentMs = msSince(closedAt);
      const auto sent = S.fleet->sendActorUpdate(p3.token.gameTokenId, {kChunk, p3.uuid, bytesOf(pose)});
      ++sendResults[sent.ok() ? "Ok" : errcName(sent.error())];
      (void)S.fleet->flushSends();
      w.until([&] {
        if (S.fleet->moveOrdered() != moveOrdered) {
          moveOrdered = !moveOrdered;
          if (moveOrdered && moveOrderedAtMs < 0) moveOrderedAtMs = msSince(closedAt);
          note(std::string("moveOrdered() became ") + (moveOrdered ? "true" : "false"));
        }
        if (S.fleet->refreshDue() != refreshDue) {
          refreshDue = !refreshDue;
          if (refreshDue && refreshDueAtMs < 0) refreshDueAtMs = msSince(closedAt);
          note(std::string("refreshDue() became ") + (refreshDue ? "true" : "false"));
        }
        if (S.fleet->state() != state) {
          state = S.fleet->state();
          note(std::string("state() became ") + stateName(state));
        }
        return false;
      }, 1000);
      if (Y.updatesWith(pose) > 0) {
        lastArrivedSentMs = sentMs;
        ++arrivedCount;
        if (sentMs >= 5000) ++arrivedLate;
      }
    }
    const auto statsAfter = S.fleet->stats();
    std::string sends;
    for (const auto& [name, count] : sendResults) sends += (sends.empty() ? "" : ", ") + name + " x" + std::to_string(count);
    say("   " + S.name + "'s sends for player 3 after the close returned: " + sends);
    say("   of those, Y received " + std::to_string(arrivedCount) +
        (arrivedCount > 0 ? " (the last one that arrived was sent " + std::to_string(lastArrivedSentMs) +
                                " ms after the close)"
                          : ""));
    std::string frames;
    for (int type = 0; type < 256; ++type) {
      const auto more = statsAfter.messagesReceivedByType[type] - statsBefore.messagesReceivedByType[type];
      if (more) frames += (frames.empty() ? "" : ", ") + std::string("type ") + std::to_string(type) + " x" + std::to_string(more);
    }
    say("   frames the realtime server sent " + S.name + " after the close: " +
        (frames.empty() ? std::string("none") : frames));
    say("   datagrams received " + std::to_string(statsAfter.datagramsReceived - statsBefore.datagramsReceived) +
        ", signed bundles " + std::to_string(statsAfter.signedBundlesReceived - statsBefore.signedBundlesReceived) +
        ", HMAC failures " + std::to_string(statsAfter.hmacFailures - statsBefore.hmacFailures) +
        ", malformed " + std::to_string(statsAfter.malformed - statsBefore.malformed) +
        ", reconnects " + std::to_string(statsAfter.reconnects - statsBefore.reconnects) +
        "; capabilities sent " +
        std::to_string(statsAfter.messagesSentByType[29] - statsBefore.messagesSentByType[29]) +
        " times and acknowledged " + std::to_string(statsAfter.boundControlAcks - statsBefore.boundControlAcks) +
        ", FLEET_CARRY sent " +
        std::to_string(statsAfter.messagesSentByType[49] - statsBefore.messagesSentByType[49]));
    std::map<std::string, int> errorKinds;
    std::int64_t firstErrorMs = -1;
    for (std::size_t i = errorsBefore; i < S.errors.size(); ++i) {
      const auto& e = S.errors[i];
      if (firstErrorMs < 0)
        firstErrorMs = std::chrono::duration_cast<std::chrono::milliseconds>(e.at - closedAt).count();
      ++errorKinds["code " + std::to_string(static_cast<int>(e.code)) + (e.verified ? " verified" : " unverified")];
    }
    std::string errors;
    for (const auto& [kind, count] : errorKinds) errors += (errors.empty() ? "" : ", ") + kind + " x" + std::to_string(count);
    say("   error frames handed to genericError: " + (errors.empty() ? std::string("none") : errors) +
        (firstErrorMs >= 0 ? " (the first " + std::to_string(firstErrorMs) + " ms after the close)" : ""));
    for (std::size_t i = statusesBefore; i < S.statuses.size(); ++i) {
      say("   status handler: " + std::string(stateName(S.statuses[i].state)) + " at +" +
          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(S.statuses[i].at - closedAt).count()) +
          " ms");
    }
    for (std::size_t i = releasedBefore; i < S.released.size(); ++i) {
      say("   released handler: player " + std::to_string(S.released[i].released.playerGameTokenId) + ", reason " +
          std::to_string(S.released[i].released.wireReason));
    }
    say(std::string("   at the end: state() ") + stateName(S.fleet->state()) + ", moveOrdered() " +
        (S.fleet->moveOrdered() ? "true" : "false") + ", refreshDue() " +
        (S.fleet->refreshDue() ? "true" : "false") + ", carries(player 3) " +
        (S.fleet->carries(p3.token.gameTokenId) ? "true" : "false"));

    // An ended session no longer speaks for its players, and takes none: a carry it sends
    // for a player Y carries leaves Y's carry standing.
    FLEET_CHECK(arrivedLate == 0, "nothing an ended session sends for a player is delivered");
    if (watchdogMs == 0) {
      const std::size_t yReleased = Y.released.size();
      const CarryOutcome taken = carry(w, S, p2);
      say("   a carry of player 2 on the ended session: " + std::string(fleetCarryStatusName(taken.result.status)) +
          " after " + std::to_string(taken.ms) + " ms");
      FLEET_CHECK(!taken.result.carried(), "an ended session carries nobody new");
      w.spin(500);
      FLEET_CHECK(Y.fleet->carries(p2.token.gameTokenId) && Y.releasedOf(p2.token.gameTokenId, yReleased) == nullptr,
                  "and the session that carries the player keeps it");
    }
    if (watchdogMs > 0) {
      // Nothing comes back to an ended session, so the silence watchdog is what tells it.
      FLEET_CHECK(moveOrderedAtMs >= 0 && refreshDueAtMs >= 0,
                  "with the silence watchdog on, the ended session is told to refresh");
      FLEET_CHECK(moveOrderedAtMs <= watchdogMs + 20000, "within the watchdog's time and one advertisement");
    }
    // The session is gone at the API: a second close answers false, a refresh NOT_FOUND.
    FLEET_CHECK(!sessions.close(*S.fleet), "a second close answers false");
    bool notFound = false;
    try {
      (void)sessions.refresh(*S.fleet);
    } catch (const graphql::CrowdyGraphQLError& e) {
      notFound = graphql::fleetSessionNotFoundOf(e.errors());
      if (!notFound) fail("refreshing an ended session was refused " + e.code() + ", not NOT_FOUND");
    }
    FLEET_CHECK(notFound, "refreshing an ended session is NOT_FOUND");
    S.fleet->close();
    S.fleet.reset();

    // The ended session's carries ended with it: the player's sign-in still stands, and
    // another of the organization's servers carries the player.
    const CarryOutcome back = carry(w, Y, p3);
    say("   Y carries player 3 again: " + std::string(fleetCarryStatusName(back.result.status)) + ", " +
        std::to_string(back.ms) + " ms");
    FLEET_CHECK(back.result.carried() && back.result.userId == p3.userId,
                "a player an ended session carried is carried again");
    p3.carrier = &Y;
  };

  E2E_SUBTEST("9b. a session ended through the API while its FleetSession is open (60 s observed)");
  endedSession(Z, 0, 60000);
  E2E_SUBTEST("9b, again with the silence watchdog on (transport.watchdogSilenceMs 20000; 45 s observed)");
  endedSession(V, 20000, 45000);

  // ---- 9c ----
  if (idleSeconds > 0) {
    E2E_SUBTEST("9c. a carried player with no input for the app's idle timeout is released, reason 5");
    FLEET_CHECK(w.until([&] { return X.releasedOf(p1.token.gameTokenId, xReleasedBefore) != nullptr; },
                        std::max(0, idleSeconds * 1000 + 90000 - static_cast<int>(msSince(quietSince)))),
                "X is told player 1 is released");
    const ReleasedSeen* told = X.releasedOf(p1.token.gameTokenId, xReleasedBefore);
    say("   released " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(told->at - p1.lastInput).count() / 1000) +
        " s after player 1's last input, reason " + fleetReleaseReasonName(told->released.reason) + " (" +
        std::to_string(told->released.wireReason) + ")");
    FLEET_CHECK(told->released.reason == FleetReleaseReason::Idle && told->released.wireReason == 5,
                "the reason is the idle timeout");
    FLEET_CHECK(!X.fleet->carries(p1.token.gameTokenId), "X no longer carries player 1");
    FLEET_CHECK(X.fleet->state() == ConnState::Connected, "the session itself stays connected");
    p1.carrier = nullptr;
  } else {
    E2E_SUBTEST("9c. the idle release: skipped (CROWDY_FLEET_IDLE_SECONDS is not set)");
    p1.quiet = false;
  }

  // ---- 10 ----
  E2E_SUBTEST("10. the hub connection a game server opens for itself");
  {
    const std::string skipped = hubStep(apiUrl);
    if (!skipped.empty()) say("   skipped: " + skipped);
    std::printf("HUB_STEP=%s\n", skipped.empty() ? "ran" : "skipped");
  }

  // ---- 11 ----
  E2E_SUBTEST("11. release every player, close both sessions on the wire and through the API");
  w.tick();
  FLEET_CHECK(X.plainCommunications == 0 && Y.plainCommunications == 0,
              "no communication arrived outside a FLEET_DELIVERY in the whole run");
  for (std::size_t i = 0; i < w.players.size(); ++i) {
    Managed& p = w.players[i];
    if (!p.carrier) continue;
    FLEET_CHECK(p.carrier->fleet->carries(p.token.gameTokenId), "a player still carried is carried");
    FLEET_CHECK(p.carrier->fleet->release(p.token.gameTokenId).ok(), "release() is sent");
    FLEET_CHECK(!p.carrier->fleet->carries(p.token.gameTokenId), "the session no longer carries the player");
    p.carrier = nullptr;
  }
  for (GameServer* s : {&X, &Y}) {
    (void)s->fleet->flushSends();
    const auto stats = s->fleet->stats();
    say("   " + s->name + ": " + std::to_string(stats.messagesSent) + " messages sent, " +
        std::to_string(stats.messagesReceived) + " received, " + std::to_string(stats.signedBundlesReceived) +
        " signed bundles, " + std::to_string(stats.hmacFailures) + " HMAC failures, " +
        std::to_string(stats.sendsFailed) + " sends failed, " + std::to_string(stats.reconnects) +
        " reconnects; capabilities sent " + std::to_string(stats.messagesSentByType[29]) +
        " times, acknowledged " + std::to_string(stats.boundControlAcks) + "; FLEET_CARRY_RESULT " +
        std::to_string(stats.messagesReceivedByType[51]) + ", FLEET_RELEASED " +
        std::to_string(stats.messagesReceivedByType[52]) + ", FLEET_DELIVERY " +
        std::to_string(stats.messagesReceivedByType[53]) + ", error frames " +
        std::to_string(stats.messagesReceivedByType[3]));
    // A live session's every advertisement is acknowledged, so silence means something.
    FLEET_CHECK(stats.boundControlAcks == stats.messagesSentByType[29],
                s->name + ": every capabilities advertisement was acknowledged");
    FLEET_CHECK(stats.hmacFailures == 0, s->name + ": every frame's HMAC held");
    FLEET_CHECK(stats.sendsFailed == 0, s->name + ": no send met a socket fault");
  }
  w.spin(300);
  X.fleet->close();
  Y.fleet->close();
  FLEET_CHECK(X.fleet->state() == ConnState::Closed && Y.fleet->state() == ConnState::Closed,
              "both FleetSessions are closed");
  for (GameServer* s : {&X, &Y}) {
    FLEET_CHECK(sessions.close(appId, s->opened.gameTokenId), "closeFleetSession ends " + s->name + "'s session (true)");
  }
  for (GameServer* s : {&X, &Y}) {
    FLEET_CHECK(!sessions.close(appId, s->opened.gameTokenId), "a second close of " + s->name + " answers false");
  }

  std::puts("e2e_fleet_session OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const graphql::CrowdyGraphQLError& e) {
    std::string detail = e.code() + ": " + e.what();
    for (const auto& d : e.errors()) detail += " [" + d.code + " " + d.path + " " + d.message + "]";
    fail("the Application API refused a call: " + detail);
  } catch (const std::exception& e) {
    fail(std::string("exception: ") + e.what());
  }
}
