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
//   CROWDY_FLEET_REALTIME_MIN  the least version the tier's realtime servers are known to
//                              run, as 0.50.1. Three steps differ between v0.50.1 and
//                              the build before it: how a session closed through the API
//                              learns of it
//                              (a signed release within 5 seconds, or 45 seconds of
//                              silence), whether a refusal arrives verified, and whether a
//                              carried player's refreshed token is carried with no send
//                              refused. Each prints which it saw. With this set to 0.50.1
//                              or later, the earlier behaviour fails the run.
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
// no setting. Every fleet session it opens (four) is closed through the API before it passes.
//
// The last step judges the realtime servers, and the SDK only for having counted: every
// session was sent nothing but its own (no notification of another app, none unsigned, no
// frame whose HMAC failed). What it prints on a failure is for Crowded Kingdoms.
//
// The players stand in chunk (7100, 7100, 0). Each send is retried a few times where UDP may
// have dropped it; a refusal or a wrong delivery fails the run.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
  std::fflush(stdout);  // what was printed so far comes first
  std::fprintf(stderr, "e2e_fleet_session FAILED: %s\n", scrub(why).c_str());
  std::exit(1);
}

#define FLEET_CHECK(cond, why)                                                         \
  do {                                                                                 \
    if (!(cond)) fail(std::string(why) + " (" #cond ", line " + std::to_string(__LINE__) + ")"); \
  } while (0)

/// CROWDY_FLEET_REALTIME_MIN as major, minor, patch; zeros when unset.
std::array<int, 3> realtimeMinimum() {
  std::array<int, 3> version{0, 0, 0};
  const std::string text = envOr("CROWDY_FLEET_REALTIME_MIN");
  std::size_t at = 0;
  if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) at = 1;
  for (int part = 0; part < 3 && at < text.size(); ++part) {
    version[static_cast<std::size_t>(part)] = std::atoi(text.c_str() + at);
    at = text.find('.', at);
    if (at == std::string::npos) break;
    ++at;
  }
  return version;
}

/// True when the tier's realtime servers are known to run at least this version.
bool realtimeAtLeast(int major, int minor, int patch) {
  return realtimeMinimum() >= std::array<int, 3>{major, minor, patch};
}

std::int64_t msSince(Clock::time_point from) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count();
}

const Clock::time_point kRunStart = Clock::now();
/// The step the run is in, for what a session logs from its network thread.
std::atomic<const char*> currentStep{"0"};

void step(const char* number, const std::string& title) {
  currentStep.store(number);
  std::printf("-- %s. %s\n", number, title.c_str());
}

/// What a session's SDK logs at WARN and above, with when and in which step: a dropped
/// notification is named there (the app it named, its message type, its chunk).
struct SessionLog final : core::ILogger {
  struct Line {
    std::int64_t atMs;
    const char* step;
    std::string text;
  };
  bool enabled(core::LogLevel level) const override { return level >= core::LogLevel::Warn; }
  void log(core::LogLevel, std::string_view message) const override {
    std::lock_guard lock(mutex);
    lines.push_back({msSince(kRunStart), currentStep.load(), std::string(message)});
  }
  std::vector<Line> all() const {
    std::lock_guard lock(mutex);
    return lines;
  }
  mutable std::mutex mutex;
  mutable std::vector<Line> lines;
};

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

struct SessionEndSeen {
  FleetSessionReleased released;
  Clock::time_point at;
};

/// One of the organization's game servers: its session at the API and on the wire.
struct GameServer {
  explicit GameServer(std::string called) : name(std::move(called)) {}

  std::string name;
  FleetSessionResponse opened;
  /// What the session's SDK logged. One per session, and it outlives the session.
  std::shared_ptr<SessionLog> log = std::make_shared<SessionLog>();
  std::unique_ptr<FleetSession> fleet;

  std::vector<Update> updates;
  std::vector<Delivery> texts;
  std::vector<ErrorSeen> errors;
  std::vector<ReleasedSeen> released;
  std::vector<StatusSeen> statuses;
  std::vector<FleetCarryResult> results;
  /// The realtime server's signed word that the session ended (v0.50.1 sends one).
  std::vector<SessionEndSeen> sessionEnds;
  /// Verified refusals the session could name a player for.
  std::vector<FleetSendRefused> refusedSends;
  /// Communications that arrived outside a FLEET_DELIVERY: there must be none.
  int plainCommunications = 0;
  /// Notifications a handler was given that named another app than the session's: the SDK
  /// drops those, so there must be none.
  int ofAnotherApp = 0;
  Clock::time_point lastOwnHeartbeat{};

  void connect() {
    // The config as fleetConfigOf makes it: the silence timeout on, 45 seconds.
    auto config = domains::fleetConfigOf(opened);
    FLEET_CHECK(config.ok(), name + ": the open answer makes a FleetConfig");
    FLEET_CHECK(config.value().transport.watchdogSilenceMs == 45000, "the silence timeout is 45 seconds");
    fleet.reset();
    log = std::make_shared<SessionLog>();
    ofAnotherApp = 0;
    sessionEnds.clear();
    refusedSends.clear();
    fleet = std::make_unique<FleetSession>(config.value(), core::opensslCrypto(), core::systemClock(), *log);
    FleetHandlers h;
    h.any = [this, app = config.value().appId](const SpatialNotification& n, FleetRecipients) {
      if (n.appId != app) ++ofAnotherApp;
    };
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
    h.sessionReleased = [this](const FleetSessionReleased& r) { sessionEnds.push_back({r, Clock::now()}); };
    h.sendRefused = [this](const FleetSendRefused& r) { refusedSends.push_back(r); };
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

/// The run: two servers, a third for the ended-session step, four players.
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

const char* stateName(ConnState s);

/// An actor update `from` sends for `player` must reach `to`: `pose` is sent until it does,
/// up to ten times three seconds apart (UDP may drop one, and a session's first messages in
/// a chunk can be refused while its permissions there load). When it never arrives, what
/// both sessions saw meanwhile is printed with the failure.
void requireDelivery(World& w, GameServer& from, GameServer& to, const Managed& player,
                     const std::string& pose, const std::string& what) {
  const std::size_t errorsBefore = from.errors.size();
  const auto sentBefore = from.fleet->stats();
  const auto before = to.fleet->stats();
  const int updatesBefore = to.updatesFrom(player.uuid);
  std::map<std::string, int> sends;
  const bool arrived = w.retry(
      [&](int) {
        const auto sent = from.fleet->sendActorUpdate(player.token.gameTokenId, {kChunk, player.uuid, bytesOf(pose)});
        ++sends[sent.ok() ? "Ok" : errcName(sent.error())];
        (void)from.fleet->flushSends();
      },
      [&] { return to.updatesWith(pose) > 0; }, 10, 3000);
  if (arrived) return;
  std::string detail = what + ". " + from.name + "'s sends returned";
  for (const auto& [name, count] : sends) detail += " " + name + " x" + std::to_string(count);
  std::map<std::string, int> refusals;
  for (std::size_t i = errorsBefore; i < from.errors.size(); ++i) {
    ++refusals["code " + std::to_string(static_cast<int>(from.errors[i].code))];
  }
  detail += "; " + from.name + " (" + stateName(from.fleet->state()) + ", carries the player: " +
            (from.fleet->carries(player.token.gameTokenId) ? "yes" : "no") + ", on server " +
            from.opened.server.serverId + ") was answered";
  if (refusals.empty()) detail += " no error frame";
  for (const auto& [code, count] : refusals) detail += " " + code + " x" + std::to_string(count);
  const auto sentAfter = from.fleet->stats();
  detail += " and sent " + std::to_string(sentAfter.messagesSentByType[128] - sentBefore.messagesSentByType[128]) +
            " actor updates in " + std::to_string(sentAfter.datagramsSent - sentBefore.datagramsSent) +
            " datagrams (" + std::to_string(sentAfter.sendsFailed - sentBefore.sendsFailed) + " failed)";
  const auto after = to.fleet->stats();
  detail += "; " + to.name + " (" + stateName(to.fleet->state()) + ", on server " + to.opened.server.serverId +
            ") received " + std::to_string(after.messagesReceived - before.messagesReceived) + " messages in " +
            std::to_string(after.datagramsReceived - before.datagramsReceived) + " datagrams meanwhile, " +
            std::to_string(to.updatesFrom(player.uuid) - updatesBefore) + " of them updates of this player's actor; it dropped " +
            std::to_string(after.wrongApp - before.wrongApp) + " of another app, " +
            std::to_string(after.unsignedNotifications - before.unsignedNotifications) + " unsigned, " +
            std::to_string(after.hmacFailures - before.hmacFailures) + " for their HMAC, " +
            std::to_string(after.malformed - before.malformed) + " malformed, " +
            std::to_string(after.ringDropped - before.ringDropped) + " for a full ring";
  fail(detail);
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
  // What realtime servers v0.50.1 changed is required only of a tier known to run it.
  const bool v0501 = realtimeAtLeast(0, 50, 1);
  say(v0501 ? "the tier's realtime servers are at least v0.50.1 (CROWDY_FLEET_REALTIME_MIN): its behaviour is required"
            : "CROWDY_FLEET_REALTIME_MIN is not 0.50.1 or later: what differs from v0.50.1 is reported, and does not fail the run");

  auto http = std::make_shared<CountingTransport>(graphql::makeCurlTransport());
  ClientConfig c;
  c.httpUrl = apiUrl;
  c.transport = http;
  CrowdyClient api(std::move(c));
  api.setToken(orgToken);
  auto& sessions = api.admin().fleetSessions();

  World w;
  w.tag = runSuffix();
  GameServer X("X"), Y("Y"), Z("Z");
  w.servers = {&X, &Y, &Z};
  /// What a session that is gone was sent that was not its own: (name, server, stats).
  struct Retired {
    std::string name;
    domains::ServerAssignment server;
    FleetSession::Stats stats;
    std::shared_ptr<SessionLog> log;
    int ofAnotherApp;
  };
  std::vector<Retired> retired;

  // ---- 1 ----
  step("1", "two game servers open a fleet session each");
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
  step("2", "the organization signs four of its players in (mintManagedPlayerToken)");
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
  step("3", "both sessions connect to their realtime server");
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
  step("4", "X carries players 0 and 1, Y carries players 2 and 3");
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
  step("5", "state: an actor update arrives once, plain, and a session is not sent its own players'");
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
  step("6", "text: delivered only inside FLEET_DELIVERY, listing who may receive it");
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
  step("7", "a crossing: Y carries player 0, with no Application API request");
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
    requireDelivery(w, Y, X, p0, "crossed " + w.tag, "after the crossing X receives player 0's state from Y");
  }

  // ---- 8 ----
  step("8", "a refusal: player 1 may not send under player 0's actor");
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
    FLEET_CHECK(X.fleet->carries(p1.token.gameTokenId) && X.fleet->state() == ConnState::Connected &&
                    !X.fleet->moveOrdered() && !X.fleet->refreshDue(),
                "a refusal changes nothing in the session");
    std::printf("REFUSALS_VERIFIED=%s\n", refusal->verified ? "true" : "false");
    // Realtime servers v0.50.1 send a fleet session its refusals inside its signed bundles.
    if (v0501) FLEET_CHECK(refusal->verified, "a refusal from realtime servers v0.50.1 is verified");
    // A verified refusal names the player whose send it was: X has sent nothing under that
    // sequence number for anyone else in the last seconds.
    const FleetSendRefused* named = nullptr;
    for (const auto& r : X.refusedSends) {
      if (r.sequence == refusal->sequence) named = &r;
    }
    if (refusal->verified) {
      FLEET_CHECK(named != nullptr, "sendRefused names the player of a verified refusal");
      FLEET_CHECK(named->playerGameTokenId == p1.token.gameTokenId && named->userId == p1.userId &&
                      named->code == wire::ErrorCode::Unauthorized && !named->playerTokenExpired(),
                  "the player it names is player 1");
      say("   sendRefused named player 1");
    } else {
      FLEET_CHECK(named == nullptr, "an unverified refusal names no player");
      say("   the refusal was unverified, so sendRefused named nobody (realtime servers before v0.50.1)");
    }
  }

  // ---- 9 ----
  step("9", "refresh: the same token and id, a later expiry, the same server, the carries stand");
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
    requireDelivery(w, X, Y, p1, "after refresh " + w.tag,
                    "a send for the carried player still arrives on the other session");
    p1.lastInput = Clock::now();
  }

  // ---- 9a ----
  step("9a", "a carried player's refreshed token: carried throughout, and no send refused");
  {
    Managed& p = p2;
    const std::int64_t oldId = p.token.gameTokenId;
    // refreshAppToken, as the player (the app token is its bearer). A carried player has no
    // session of its own, so the refresh names no realtime server.
    ClientConfig pc;
    pc.httpUrl = apiUrl;
    CrowdyClient asPlayer(std::move(pc));
    asPlayer.setToken(p.token.token);
    const auto fresh = whenNotBusy([&] { return asPlayer.portal().refresh(/*install=*/false); });
    keepSecret(fresh.token);
    FLEET_CHECK(fresh.token.size() == 64, "refreshAppToken answers a 64-character token");
    const std::int64_t newId = fresh.gameTokenIdInt64().value_or(0);
    FLEET_CHECK(newId > 0, "and its gameTokenId");
    FLEET_CHECK(fresh.token != p.token.token, "and it is a new token");
    say("   refreshAppToken answered " +
        std::string(newId != oldId ? "a new gameTokenId, " + std::to_string(newId) : "the same gameTokenId"));

    const std::size_t errorsBefore = Y.errors.size(), releasedBefore = Y.released.size();
    const auto statsBefore = Y.fleet->stats();
    p.quiet = true;  // this step sends for the player itself, every 20 ms
    bool done = false;
    FleetCarryResult result;
    std::int64_t carriedMs = -1;
    const auto start = Clock::now();
    FLEET_CHECK(Y.fleet
                    ->carry(PlayerToken{fresh.token, newId, oldId},
                            [&](const FleetCarryResult& r) {
                              result = r;
                              carriedMs = msSince(start);
                              done = true;
                            })
                    .ok(),
                "carry() with the refreshed token and `replaces` is accepted");
    std::vector<std::string> poses;
    std::map<std::string, int> refusedHere;
    for (int n = 0; msSince(start) < 2000; ++n) {
      const std::string pose = "refreshed " + w.tag + " " + std::to_string(n);
      // Either id names the player while the carry waits; the session puts the right one on
      // the wire. Once the carry is confirmed the old id names nothing.
      const std::int64_t id = (!done && n % 2 == 0) ? oldId : newId;
      const auto sent = Y.fleet->sendActorUpdate(id, {kChunk, p.uuid, bytesOf(pose)});
      if (sent.ok()) poses.push_back(pose);
      else ++refusedHere[errcName(sent.error())];
      (void)Y.fleet->flushSends();
      w.spin(20);
    }
    FLEET_CHECK(done, "the refreshed token's carry has a result");
    say("   the carry: " + std::string(fleetCarryStatusName(result.status)) + " in " + std::to_string(carriedMs) + " ms");
    FLEET_CHECK(result.carried() && result.userId == p.userId && result.playerGameTokenId == newId,
                "the refreshed token's carry ends Carried, for the same user");
    std::string here;
    for (const auto& [name, count] : refusedHere) here += " " + name + " x" + std::to_string(count);
    FLEET_CHECK(refusedHere.empty(), "the player stayed carried: the session accepted every send (" + here + " )");
    FLEET_CHECK(Y.fleet->carries(newId), "the session carries the player under the new id");
    if (newId != oldId) {
      FLEET_CHECK(!Y.fleet->carries(oldId), "and the old id names nothing");
      FLEET_CHECK(Y.fleet->sendActorUpdate(oldId, {kChunk, p.uuid, bytesOf("old")}).error() == Errc::NotCarried,
                  "a send for the old id is NotCarried");
    }
    p.token = PlayerToken{fresh.token, newId};
    p.quiet = false;
    p.lastInput = Clock::now();
    // A refusal can take the realtime server two seconds; after that, what was sent arrived.
    w.spin(2500);
    FLEET_CHECK(Y.releasedOf(oldId, releasedBefore) == nullptr && Y.releasedOf(newId, releasedBefore) == nullptr,
                "no release was reported: the carry under the old token stood until the new one took effect");
    int reached = 0;
    for (const auto& pose : poses) reached += X.updatesWith(pose) > 0;
    std::map<std::string, int> refusals;
    for (std::size_t i = errorsBefore; i < Y.errors.size(); ++i) {
      ++refusals["code " + std::to_string(static_cast<int>(Y.errors[i].code)) +
                 (Y.errors[i].verified ? " verified" : " unverified")];
    }
    std::string answered;
    for (const auto& [kind, count] : refusals) answered += (answered.empty() ? "" : ", ") + kind + " x" + std::to_string(count);
    const auto statsAfter = Y.fleet->stats();
    const int refused = static_cast<int>(Y.errors.size() - errorsBefore);
    say("   " + std::to_string(poses.size()) + " sends for the player in 2 s around the carry: " +
        std::to_string(reached) + " reached X; the realtime server refused " + std::to_string(refused) +
        (answered.empty() ? "" : " (" + answered + ")") + "; late frames about the old id: " +
        std::to_string(statsAfter.replacedIdFrames - statsBefore.replacedIdFrames));
    std::printf("REFRESHED_TOKEN_REFUSED_SENDS=%d\n", refused);
    FLEET_CHECK(reached > 0, "the player's sends reach the other session");
    if (v0501) {
      FLEET_CHECK(refused == 0, "no send for the player was refused around the refreshed token's carry");
      FLEET_CHECK(reached * 10 >= static_cast<int>(poses.size()) * 9, "and they reached the other session");
    }
  }

  // ---- 9b ----
  // Player 1 goes quiet from here: with CROWDY_FLEET_IDLE_SECONDS its idle release is step 9c.
  p1.quiet = true;
  const auto quietSince = Clock::now();
  const std::size_t xReleasedBefore = X.released.size();

  // A third server takes player 3 from Y, its session is closed through the API, and its
  // FleetSession goes on sending for player 3. Realtime servers v0.50.1 tell the session,
  // signed, at once (COMMAND_SESSION_RELEASED, reason 0); the build before it sends nothing, and the
  // silence timeout makes the refresh due about 45 seconds later. Either way the session no
  // longer speaks for the player, the refresh answers NOT_FOUND, and a new session carries
  // the player again.
  step("9b", "a session ended through the API: a refresh is due, NOT_FOUND, a new session");
  {
    GameServer& S = Z;
    S.opened = whenNotBusy([&] { return sessions.open(appId); });
    wellFormed(S);
    S.connect();
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
    requireDelivery(w, S, Y, p3, "live " + S.name + " " + w.tag,
                    S.name + "'s sends for player 3 reach Y before the session is ended");
    w.spin(2000);            // some ordinary traffic first, to compare with
    p3.carrier = nullptr;    // from here this step sends for player 3 itself, numbered
    FLEET_CHECK(!S.fleet->moveOrdered() && !S.fleet->refreshDue(), "nothing is due on a live session");

    const auto statsBefore = S.fleet->stats();
    const std::size_t errorsBefore = S.errors.size(), statusesBefore = S.statuses.size(),
                      releasedBefore = S.released.size();
    FLEET_CHECK(sessions.close(*S.fleet), "closeFleetSession ends " + S.name + "'s session (true)");
    const auto closedAt = Clock::now();
    say("   before the close the realtime server had sent " + S.name + " " +
        std::to_string(statsBefore.datagramsReceived) + " datagrams (" +
        std::to_string(statsBefore.boundControlAcks) + " capability acknowledgements). Closed through the API; " +
        S.name + "'s FleetSession keeps sending for player 3, once a second");

    std::map<std::string, int> sendResults;
    int arrivedCount = 0, arrivedLate = 0;
    std::int64_t lastArrivedSentMs = -1, moveOrderedAtMs = -1, refreshDueAtMs = -1;
    bool moveOrdered = false, refreshDue = false;
    ConnState state = S.fleet->state();
    const auto note = [&](const std::string& what) {
      say("   +" + std::to_string(msSince(closedAt)) + " ms: " + what);
    };
    // Until 3 seconds after the refresh came due, or 60 seconds.
    for (int n = 0; msSince(closedAt) < 60000 &&
                    (refreshDueAtMs < 0 || msSince(closedAt) < refreshDueAtMs + 3000);
         ++n) {
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
    for (std::size_t i = errorsBefore; i < S.errors.size(); ++i) {
      const auto& e = S.errors[i];
      ++errorKinds["code " + std::to_string(static_cast<int>(e.code)) + (e.verified ? " verified" : " unverified")];
    }
    std::string errors;
    for (const auto& [kind, count] : errorKinds) errors += (errors.empty() ? "" : ", ") + kind + " x" + std::to_string(count);
    say("   error frames handed to genericError: " + (errors.empty() ? std::string("none") : errors));
    for (std::size_t i = statusesBefore; i < S.statuses.size(); ++i) {
      say("   status handler: " + std::string(stateName(S.statuses[i].state)) + " at +" +
          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(S.statuses[i].at - closedAt).count()) +
          " ms");
    }
    for (std::size_t i = releasedBefore; i < S.released.size(); ++i) {
      say("   released handler: player " + std::to_string(S.released[i].released.playerGameTokenId) + ", reason " +
          std::to_string(S.released[i].released.wireReason));
    }
    for (const auto& line : S.log->all()) {
      if (line.text.find("a refresh is due") != std::string::npos) say("   the SDK logged: " + line.text);
    }
    say(std::string("   at the end: state() ") + stateName(S.fleet->state()) + ", moveOrdered() " +
        (S.fleet->moveOrdered() ? "true" : "false") + ", refreshDue() " +
        (S.fleet->refreshDue() ? "true" : "false") + ", carries(player 3) " +
        (S.fleet->carries(p3.token.gameTokenId) ? "true" : "false"));

    // An ended session no longer speaks for its players.
    FLEET_CHECK(arrivedLate == 0, "nothing an ended session sends for a player is delivered");
    // A refresh is due, by the server's signed word or by its silence, and the session keeps
    // its socket and its state meanwhile.
    FLEET_CHECK(refreshDueAtMs >= 0 && moveOrderedAtMs >= 0, "a refresh is due on an ended session");
    FLEET_CHECK(statsAfter.reconnects == statsBefore.reconnects && S.fleet->state() == ConnState::Connected,
                "the session keeps its socket and its state");
    const bool told = !S.sessionEnds.empty();
    if (told) {
      const auto& end = S.sessionEnds.front();
      const auto toldMs = std::chrono::duration_cast<std::chrono::milliseconds>(end.at - closedAt).count();
      say("   the realtime server told the session, signed: COMMAND_SESSION_RELEASED reason " +
          std::to_string(end.released.wireReason) + " (" + fleetSessionReleaseReasonName(end.released.reason) +
          "), handled " + std::to_string(toldMs) + " ms after the close; refreshDue() was true " +
          std::to_string(refreshDueAtMs) + " ms after it (realtime servers v0.50.1)");
      FLEET_CHECK(S.sessionEnds.size() == 1, "it is told once");
      FLEET_CHECK(end.released.reason == FleetSessionReleaseReason::Deauthorized && end.released.wireReason == 0,
                  "the reason is 0: the token is no longer authorized");
      FLEET_CHECK(refreshDueAtMs <= 5000, "within 5 seconds of the close");
      FLEET_CHECK(!S.fleet->carries(p3.token.gameTokenId) && statsAfter.sessionReleases == 1,
                  "and the session carries nobody");
    } else {
      say("   the realtime server sent the session nothing; the silence timeout made the refresh due " +
          std::to_string(refreshDueAtMs) + " ms after the close (realtime servers before v0.50.1)");
      // (One acknowledgement may have been on its way when the session was closed.)
      FLEET_CHECK(statsAfter.boundControlAcks - statsBefore.boundControlAcks <= 1 &&
                      statsAfter.messagesSentByType[29] - statsBefore.messagesSentByType[29] >= 3,
                  "an ended session's capabilities are not acknowledged");
      FLEET_CHECK(refreshDueAtMs >= 30000 && refreshDueAtMs <= 50000, "about 45 seconds after the close");
    }
    if (v0501) FLEET_CHECK(told, "realtime servers v0.50.1 tell a closed session that it ended");
    std::printf("ENDED_SESSION_SIGN=%s\n", told ? "released" : "silence");
    std::printf("ENDED_SESSION_REFRESH_DUE_MS=%lld\n", static_cast<long long>(refreshDueAtMs));
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
    retired.push_back({S.name + " (ended)", S.opened.server, S.fleet->stats(), S.log, S.ofAnotherApp});
    S.fleet->close();
    S.fleet.reset();

    // So the game server opens a new session and carries its players again.
    S.opened = whenNotBusy([&] { return sessions.open(appId); });
    wellFormed(S);
    S.connect();
    FLEET_CHECK(w.until([&] { return S.fleet->state() == ConnState::Connected; }, 15000),
                S.name + "'s new session is connected");
    const CarryOutcome back = carry(w, S, p3);
    say("   " + S.name + "'s new session carries player 3 again: " +
        std::string(fleetCarryStatusName(back.result.status)) + ", " + std::to_string(back.ms) + " ms");
    FLEET_CHECK(back.result.carried() && back.result.userId == p3.userId,
                "a player an ended session carried is carried again by a new session");
    p3.carrier = &S;
    requireDelivery(w, S, Y, p3, "again " + S.name + " " + w.tag,
                    "and its sends for the player reach the other session");
  }

  // ---- 9c ----
  if (idleSeconds > 0) {
    step("9c", "a carried player with no input for the app's idle timeout is released, reason 5");
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
    step("9c", "the idle release: skipped (CROWDY_FLEET_IDLE_SECONDS is not set)");
    p1.quiet = false;
  }

  // ---- 10 ----
  step("10", "the hub connection a game server opens for itself");
  {
    const std::string skipped = hubStep(apiUrl);
    if (!skipped.empty()) say("   skipped: " + skipped);
    std::printf("HUB_STEP=%s\n", skipped.empty() ? "ran" : "skipped");
  }

  // ---- 11 ----
  step("11", "release every player, close every session on the wire and through the API");
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
  for (GameServer* s : {&X, &Y, &Z}) {
    (void)s->fleet->flushSends();
    const auto stats = s->fleet->stats();
    say("   " + s->name + ": " + std::to_string(stats.messagesSent) + " messages sent, " +
        std::to_string(stats.messagesReceived) + " received, " + std::to_string(stats.signedBundlesReceived) +
        " signed bundles, " + std::to_string(stats.sendsFailed) + " sends failed, " +
        std::to_string(stats.reconnects) + " reconnects; capabilities sent " +
        std::to_string(stats.messagesSentByType[29]) + " times, acknowledged " +
        std::to_string(stats.boundControlAcks) + "; FLEET_CARRY_RESULT " +
        std::to_string(stats.messagesReceivedByType[51]) + ", FLEET_RELEASED " +
        std::to_string(stats.messagesReceivedByType[52]) + ", FLEET_DELIVERY " +
        std::to_string(stats.messagesReceivedByType[53]) + ", error frames " +
        std::to_string(stats.messagesReceivedByType[3]));
    // A live session's every advertisement is acknowledged, so silence means something.
    FLEET_CHECK(stats.boundControlAcks == stats.messagesSentByType[29],
                s->name + ": every capabilities advertisement was acknowledged");
    FLEET_CHECK(stats.sendsFailed == 0, s->name + ": no send met a socket fault");
    FLEET_CHECK(!s->fleet->moveOrdered(), s->name + ": a live session was never taken for gone");
  }
  w.spin(300);
  for (GameServer* s : {&X, &Y, &Z}) {
    s->fleet->close();
    FLEET_CHECK(s->fleet->state() == ConnState::Closed, s->name + ": the FleetSession is closed");
  }
  for (GameServer* s : {&X, &Y, &Z}) {
    FLEET_CHECK(sessions.close(appId, s->opened.gameTokenId), "closeFleetSession ends " + s->name + "'s session (true)");
  }
  for (GameServer* s : {&X, &Y, &Z}) {
    FLEET_CHECK(!sessions.close(appId, s->opened.gameTokenId), "a second close of " + s->name + " answers false");
  }

  // ---- 12 ----
  // Checked last and by itself, because it judges the realtime servers and not the steps
  // above: a session is in one app, and everything it is sent is signed. The SDK drops and
  // counts what is not; this says whether there was any.
  step("12", "the realtime server sent each session nothing but its own");
  {
    std::vector<Retired> judged;
    for (GameServer* s : {&X, &Y, &Z})
      judged.push_back({s->name, s->opened.server, s->fleet->stats(), s->log, s->ofAnotherApp});
    judged.insert(judged.end(), retired.begin(), retired.end());
    // The SDK's part first: whatever the servers sent, no handler was given another app's.
    for (const auto& j : judged) {
      FLEET_CHECK(j.ofAnotherApp == 0, j.name + ": THE SDK gave a handler a notification of another app");
    }
    std::vector<std::string> problems;
    for (const auto& j : judged) {
      say("   " + j.name + ": another app's " + std::to_string(j.stats.wrongApp) + ", unsigned " +
          std::to_string(j.stats.unsignedNotifications) + ", HMAC failures " + std::to_string(j.stats.hmacFailures));
      std::printf("NOT_ITS_OWN %s wrongApp=%llu unsigned=%llu hmacFailures=%llu\n", j.name.c_str(),
                  static_cast<unsigned long long>(j.stats.wrongApp),
                  static_cast<unsigned long long>(j.stats.unsignedNotifications),
                  static_cast<unsigned long long>(j.stats.hmacFailures));
      if (j.stats.wrongApp == 0 && j.stats.unsignedNotifications == 0 && j.stats.hmacFailures == 0) continue;
      // Which session, on which server, how many; and from the SDK's log the app each one
      // named, its message type, its chunk, and when in the run it arrived. The log names
      // the first and then each doubling of the count.
      std::string problem = j.name + " (app " + appId + ", on realtime server " + j.server.serverId + " at " +
                            j.server.ip4 + ":" + std::to_string(j.server.clientPort) + ") was sent " +
                            std::to_string(j.stats.wrongApp) + " notifications of another app, " +
                            std::to_string(j.stats.unsignedNotifications) + " that nothing signed and " +
                            std::to_string(j.stats.hmacFailures) + " frames whose HMAC failed";
      int shown = 0;
      for (const auto& line : j.log->all()) {
        if (line.text.find("dropped a notification") == std::string::npos) continue;
        if (shown == 0) {
          problem += ". The first arrived +" + std::to_string(line.atMs) + " ms into the run, in step " + line.step;
        }
        std::string named = line.text;
        if (const auto cut = named.find(". The realtime server"); cut != std::string::npos) named.resize(cut);
        if (++shown <= 12) problem += "\n      +" + std::to_string(line.atMs) + " ms, step " + line.step + ": " + named;
      }
      problems.push_back(std::move(problem));
    }
    if (!problems.empty()) {
      std::string all = "the realtime server sent a session what is not its own. The SDK dropped and counted "
                        "it, and no handler saw it. NOT THE SDK; for Crowded Kingdoms:";
      for (const auto& problem : problems) all += "\n   " + problem;
      fail(all);
    }
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
