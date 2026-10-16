// The Game Kit against ck-exec's golden record of its starter hubs
// (tools/parity/fixtures/kit-protocol.json, a copy of ck-exec
// crates/ckx-kit-protocol/kit-protocol.json, as CrowdyJS's test/unit/fixtures has it):
//
// - the protocol's own cases: kit messages, refusal codes, shard keys, realtime events and actor
//   states read as ck-exec wrote them;
// - each starter's script replayed through a gateway that answers only the next recorded step:
//   every call a kit module makes must be that step's method and arguments from that step's
//   player, and it gets the hub's real reply; the hub's real topic messages are pushed to whoever
//   subscribes. CrowdyJS's game-kit.test.mjs plays the same scripts;
// - the runtime's pacing and resends, private state, the topology's checks and emotes.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/kit/economy.hpp"
#include "crowdy/kit/genres.hpp"
#include "crowdy/kit/mirror.hpp"
#include "crowdy/kit/modules.hpp"
#include "crowdy/kit/protocol.hpp"
#include "crowdy/kit/realtime.hpp"
#include "crowdy/kit/runtime.hpp"
#include "crowdy/kit/topology.hpp"
#include "crowdy/kit/wire.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::kit;
using domains::ExecStatus;
using graphql::Json;
using graphql::JVal;
using graphql::WebSocketEvent;
using graphql::WebSocketEventKind;

namespace {

Json readRecord() {
  std::ifstream in(std::string(CROWDY_PARITY_FIXTURE_DIR) + "/kit-protocol.json");
  std::stringstream text;
  text << in.rdbuf();
  Json record = Json::parse(text.str());
  CHECK(record.ok());
  return record;
}

const Json& record() {
  static const Json r = readRecord();
  return r;
}

/// `crate`'s script; `nodeType` picks one when the crate serves two types.
Json starter(std::string_view crate, std::string_view nodeType = {}) {
  Json found;
  record()["starters"].forEach([&](const Json& s) {
    if (s["crate"].asStringView() == crate && (nodeType.empty() || s["node_type"].asStringView() == nodeType)) found = s;
  });
  CHECK(found.ok());
  return found;
}

std::string unhex(std::string_view h) {
  std::string out;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
  return out;
}

/// Deep equality of two JSON values, numbers by value.
bool same(const Json& a, const Json& b) {
  if (a.isNull() || !a.ok()) return b.isNull() || !b.ok();
  if (a.isNumber()) return b.isNumber() && std::fabs(a.asDouble() - b.asDouble()) < 1e-9;
  if (a.isString()) return b.isString() && a.asStringView() == b.asStringView();
  if (a.isBool()) return b.isBool() && a.asBool() == b.asBool();
  if (a.isArray()) {
    if (!b.isArray() || a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!same(a.at(i), b.at(i))) return false;
    }
    return true;
  }
  if (a.isObject()) {
    if (!b.isObject() || a.size() != b.size()) return false;
    bool ok = true;
    a.forEachMember([&](std::string_view k, const Json& v) { ok = ok && same(v, b[k]); });
    return ok;
  }
  return false;
}

/// `v` without its `action_id`; an object left empty is null.
Json withoutActionId(const Json& v) {
  if (!v.isObject()) return v;
  std::string text = "{";
  bool first = true;
  v.forEachMember([&](std::string_view k, const Json& m) {
    if (k == "action_id") return;
    if (!first) text.push_back(',');
    first = false;
    text += JVal(std::string(k)).dump() + ":" + m.dump();
  });
  text.push_back('}');
  return first ? Json() : Json::parse(text);
}

// ---- a fake transport, as exec_test has it ----

struct FakeConn : graphql::IWebSocketConnection {
  std::string url;
  std::mutex mu;
  graphql::WebSocketEventCallback cb;
  std::vector<std::string> sent;

  void start(graphql::WebSocketEventCallback c) override {
    std::lock_guard lock(mu);
    cb = std::move(c);
  }
  Status send(graphql::WebSocketFrame f) override {
    std::lock_guard lock(mu);
    sent.push_back(std::move(f.payload));
    return Errc::Ok;
  }
  void close(std::uint16_t, std::string_view) override {}
  void emit(WebSocketEvent ev) {
    graphql::WebSocketEventCallback c;
    {
      std::lock_guard lock(mu);
      c = cb;
    }
    if (c) c(std::move(ev));
  }
  void open() {
    WebSocketEvent ev;
    ev.kind = WebSocketEventKind::Open;
    emit(ev);
  }
  void deliver(std::string bytes) {
    WebSocketEvent ev;
    ev.kind = WebSocketEventKind::Frame;
    ev.frame = {graphql::WebSocketFrameKind::Binary, std::move(bytes)};
    emit(ev);
  }
  void drop() {
    WebSocketEvent ev;
    ev.kind = WebSocketEventKind::Close;
    ev.close.code = 1006;
    emit(ev);
  }
  std::vector<std::string> frames() {
    std::lock_guard lock(mu);
    return sent;
  }
};

struct FakeTransport : graphql::IWebSocketTransport {
  std::mutex mu;
  std::vector<std::shared_ptr<FakeConn>> conns;
  std::shared_ptr<graphql::IWebSocketConnection> createConnection(const graphql::WebSocketConnectRequest& r) override {
    auto c = std::make_shared<FakeConn>();
    c->url = r.url;
    std::lock_guard lock(mu);
    conns.push_back(c);
    return c;
  }
  std::vector<std::shared_ptr<FakeConn>> all() {
    std::lock_guard lock(mu);
    return conns;
  }
};

struct Frame {
  std::uint8_t tag = 0;
  std::uint32_t rid = 0;
  std::string nodeType, key, name, payload;
};

Frame parse(const std::string& b) {
  Frame s;
  std::size_t at = 0;
  s.tag = static_cast<std::uint8_t>(b[at++]);
  for (int i = 0; i < 4; ++i) s.rid |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at++])) << (8 * i);
  if (s.tag == 0x04) return s;
  const std::size_t tl = static_cast<std::uint8_t>(b[at++]);
  s.nodeType = b.substr(at, tl);
  at += tl;
  const std::size_t kl = static_cast<std::uint8_t>(b[at]) | (static_cast<std::size_t>(static_cast<std::uint8_t>(b[at + 1])) << 8);
  at += 2;
  s.key = b.substr(at, kl);
  at += kl;
  const std::size_t nl = static_cast<std::uint8_t>(b[at++]);
  s.name = b.substr(at, nl);
  at += nl;
  s.payload = b.substr(at);
  return s;
}

std::string reply(std::uint32_t rid, std::uint8_t status, std::string payload) {
  std::string out(1, static_cast<char>(0x81));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((rid >> (8 * i)) & 0xff));
  out.push_back(static_cast<char>(status));
  return out + payload;
}

std::string push(std::string_view t, std::string_view k, std::string_view topic, std::string payload) {
  std::string out(1, static_cast<char>(0x82));
  out.push_back(static_cast<char>(t.size()));
  out.append(t);
  out.push_back(static_cast<char>(k.size() & 0xff));
  out.push_back(static_cast<char>(k.size() >> 8));
  out.append(k);
  out.push_back(static_cast<char>(topic.size()));
  out.append(topic);
  return out + payload;
}

std::uint8_t statusOf(std::string_view name) {
  static const std::pair<std::string_view, std::uint8_t> names[] = {
      {"Ok", 0}, {"AppError", 1}, {"Busy", 2}, {"Moved", 3}, {"NotFound", 4}, {"DeadlineExceeded", 5},
      {"Denied", 6}, {"RateLimited", 7}, {"Unavailable", 8}};
  for (const auto& [n, v] : names) {
    if (n == name) return v;
  }
  return 1;
}

// ---- the replay gateway ----

struct Caller {
  std::string kind;
  std::int64_t id = 0;
};

/// A gateway that plays one starter's recorded script: it answers a call only when it is the
/// next step (method, arguments without the action id, caller), with the recorded reply, and
/// pushes the step's published messages to the topics callers subscribe to.
struct Replay {
  Json script;
  std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
  std::shared_ptr<graphql::Dispatcher> dispatcher = std::make_shared<graphql::Dispatcher>();
  std::size_t at = 0;
  std::vector<std::string> refused;
  struct Peer {
    std::shared_ptr<FakeConn> conn;
    Caller caller;
    std::set<std::string> subs;
    std::size_t seen = 0;
  };
  std::vector<Peer> peers;

  explicit Replay(Json s) : script(std::move(s)) {}

  std::string topicKey(std::string_view topic) const {
    return std::string(script["node_type"].asStringView()) + '\0' + std::string(script["key"].asStringView()) + '\0' + std::string(topic);
  }

  /// A runtime whose connection is to this gateway, as `kind`-`id`.
  std::shared_ptr<KitRuntime> runtime(std::string kind, std::int64_t id, KitRuntimeOptions options = {}) {
    domains::ExecConnectOptions o;
    o.callTimeoutMs = 3000;
    options.connection = domains::ExecConnection::open(transport, dispatcher,
                                                       domains::ExecEndpoint{"ws://gw.example", kind + "-" + std::to_string(id), "h1"}, o);
    if (kind == "player") options.playerId = std::to_string(id);
    return std::make_shared<KitRuntime>("77", nullptr, dispatcher, options);
  }

  void pump() {
    auto conns = transport->all();
    for (std::size_t i = peers.size(); i < conns.size(); ++i) {
      const std::string& url = conns[i]->url;
      const std::string token = url.substr(url.find("token=") + 6);
      Caller who{token.substr(0, token.find('-')), std::stoll(token.substr(token.find('-') + 1))};
      peers.push_back(Peer{conns[i], who, {}, 0});
      conns[i]->open();
    }
    for (std::size_t p = 0; p < peers.size(); ++p) {
      auto frames = peers[p].conn->frames();
      for (; peers[p].seen < frames.size(); ++peers[p].seen) handle(p, parse(frames[peers[p].seen]));
    }
  }

  void handle(std::size_t p, const Frame& f) {
    Peer& peer = peers[p];
    if (f.tag == 0x02) {
      peer.subs.insert(std::string(f.nodeType) + '\0' + f.key + '\0' + f.name);
      peer.conn->deliver(reply(f.rid, 0, ""));
      return;
    }
    if (f.tag == 0x03) {
      peer.subs.erase(std::string(f.nodeType) + '\0' + f.key + '\0' + f.name);
      peer.conn->deliver(reply(f.rid, 0, ""));
      return;
    }
    if (f.tag != 0x01) return;
    const Json step = script["steps"].at(at);
    const Json args = withoutActionId(f.payload.empty() ? Json() : Json::fromMsgpack(f.payload));
    const Json caller = step["caller"];
    const bool fits = step.ok() && step["call"].asStringView() == f.name && f.nodeType == script["node_type"].asStringView() &&
                      f.key == script["key"].asStringView() && caller[peer.caller.kind].isNumber() &&
                      caller[peer.caller.kind].asInt64() == peer.caller.id && caller.size() == 1 &&
                      same(withoutActionId(step["args"]), args);
    if (!fits) {
      const std::string why = "step " + std::to_string(at) + " is " + step["call"].asString() + " " + step["args"].dump() + " from " +
                              caller.dump() + ", not " + f.name + " " + args.dump() + " from " + peer.caller.kind + "-" +
                              std::to_string(peer.caller.id);
      refused.push_back(why);
      peer.conn->deliver(reply(f.rid, 11, why));
      return;
    }
    ++at;
    if (step["ok"].asBool()) {
      peer.conn->deliver(reply(f.rid, 0, unhex(step["reply_msgpack"].asStringView())));
    } else {
      peer.conn->deliver(reply(f.rid, statusOf(step["status"].asStringView()), step["error"].asString()));
    }
    publish(step["published"]);
  }

  void publish(const Json& list) {
    list.forEach([&](const Json& m) {
      for (auto& peer : peers) {
        if (peer.subs.count(topicKey(m["topic"].asStringView()))) {
          peer.conn->deliver(push(script["node_type"].asStringView(), script["key"].asStringView(), m["topic"].asStringView(),
                                  unhex(m["msgpack"].asStringView())));
        }
      }
    });
  }

  /// Plays a step no client makes (an instance's call, a timer advance).
  void serverStep() {
    pump();
    const Json step = script["steps"].at(at);
    CHECK(step["advance_ms"].ok() || step["caller"]["instance"].ok());
    ++at;
    publish(step["published"]);
  }

  template <typename Fn>
  bool until(Fn&& done, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      pump();
      dispatcher->drain();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  }

  /// Starts `start` and waits for its reply.
  template <typename Start>
  KitReply wait(Start&& start) {
    std::optional<KitReply> got;
    start([&](KitReply r) { got = std::move(r); });
    CHECK(until([&] { return got.has_value(); }));
    return *got;
  }

  void done() {
    for (const auto& why : refused) std::fprintf(stderr, "refused: %s\n", why.c_str());
    CHECK(refused.empty());
    CHECK_EQ(at, script["steps"].size());
  }
};

/// The body of a recorded published kit message.
Json bodyOf(const Json& value) { return splitKitMessage(value)->body; }

// ---- the protocol's own cases ----

void testMessages() {
  record()["messages"].forEach([](const Json& m) {
    const Json value = Json::fromMsgpack(unhex(m["msgpack"].asStringView()));
    CHECK(same(value, m["value"]));
    auto split = splitKitMessage(value);
    CHECK(split.has_value());
    CHECK_EQ(split->header.v, record()["protocol"].asInt64());
    const std::string name = m["name"].asString();
    CHECK_EQ(std::string(kitMessageKindName(split->header.kind)), name == "removal" ? "delta" : name);
    for (const char* k : {"v", "epoch", "seq", "kind"}) CHECK(!split->body[k].ok());
  });
  CHECK(!splitKitMessage(Json::parse(R"({"cells":[1]})")).has_value());
}

void testRefusals() {
  record()["errors"].forEach([](const Json& e) {
    auto parsed = parseKitRefusal(e["message"].asStringView());
    if (e["code"].isNull()) {
      CHECK(!parsed.has_value());
    } else {
      CHECK(parsed.has_value());
      CHECK_EQ(parsed->code, e["code"].asString());
      CHECK_EQ(parsed->text, e["text"].asString());
    }
  });
  KitReply coded = KitReply::from(domains::ExecReply{ExecStatus::AppError, "kit:cooldown: attacking too fast"});
  CHECK_EQ(coded.code(), "cooldown");
  KitReply denied = KitReply::from(domains::ExecReply{ExecStatus::Denied, "kit:cooldown: x"});
  CHECK_EQ(denied.code(), "");
}

void testShards() {
  record()["shards"]["keys"].forEach([](const Json& k) {
    CHECK_EQ(shardKey(k["x"].asInt64(), k["z"].asInt64(), k["size"].asInt64()), k["key"].asString());
  });
  record()["shards"]["around"].forEach([](const Json& a) {
    auto keys = shardKeysAround(a["x"].asInt64(), a["z"].asInt64(), a["size"].asInt64(), static_cast<int>(a["radius"].asInt64()));
    CHECK_EQ(keys.size(), a["keys"].size());
    for (std::size_t i = 0; i < keys.size(); ++i) CHECK_EQ(keys[i], a["keys"].at(i).asString());
  });
  CHECK(parseShardKey("12,-4") == std::make_optional(std::make_pair<std::int64_t, std::int64_t>(12, -4)));
  CHECK(!parseShardKey("x").has_value());
}

void testEvents() {
  int typed = 0;
  record()["events"].forEach([&](const Json& e) {
    const std::string raw = unhex(e["bytes"].asStringView());
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(raw.data());
    auto event = parseEngineEvent(bytes, raw.size());
    CHECK(event.has_value());
    CHECK_EQ(event->eventType, e["event_type"].asInt64());
    CHECK(same(event->body, e["body"]));
    CHECK(isKitEventType(event->eventType));
    auto server = parseKitServerEvent(event->eventType, std::string_view(raw).substr(2));
    CHECK(server.has_value() && same(server->body, e["body"]));
    bool parsed = false;
    switch (event->eventType) {
      case kEventContactDamage: parsed = parseContactDamage(bytes, raw.size()).has_value(); break;
      case kEventWeather: parsed = parseWeatherEvent(bytes, raw.size()).has_value(); break;
      case kEventTurn: parsed = parseTurnEvent(bytes, raw.size()).has_value(); break;
      case kEventScore: parsed = parseScoreEvent(bytes, raw.size()).has_value(); break;
      case kEventProposal: parsed = parseProposalEvent(bytes, raw.size()).has_value(); break;
      case kEventAbility: parsed = parseAbilityEvent(bytes, raw.size()).has_value(); break;
      case kEventMovementViolation: parsed = parseMovementViolation(bytes, raw.size()).has_value(); break;
      case kEventControlPoint: parsed = parseControlPointEvent(bytes, raw.size()).has_value(); break;
      case kEventRaceTiming: parsed = parseRaceTimingEvent(bytes, raw.size()).has_value(); break;
      case kEventZoneChange: parsed = parseZoneChangeEvent(bytes, raw.size()).has_value(); break;
      case kEventEncounter: {
        auto x = parseEncounterEvent(bytes, raw.size());
        parsed = x && x->kind == e["body"]["kind"].asString() && x->wave == e["body"]["wave"].asInt64();
        break;
      }
      case kEventPossession: {
        auto x = parsePossessionEvent(bytes, raw.size());
        parsed = x && x->kind == e["body"]["kind"].asString() && x->team.value_or("") == e["body"]["team"].asString();
        break;
      }
      case kEventEmote: {
        auto x = parseEmoteEvent(bytes, raw.size());
        parsed = x && x->emote == e["body"]["emote"].asString();
        // The kit's own encoding of the same emote is the registry's body.
        const std::string ours = encodeEmoteBody(x->emote, x->targetUuid ? std::optional<std::string_view>(*x->targetUuid) : std::nullopt);
        CHECK(same(Json::parse(ours), e["body"]));
        EmotesKit emotes(nullptr);
        auto back = emotes.parse(kEventEmote, reinterpret_cast<const std::uint8_t*>(ours.data()), ours.size());
        CHECK(back && back->emote == x->emote);
        CHECK(!emotes.parse(kEventWeather, nullptr, 0).has_value());
        CHECK(emotes.send(EmoteSend{}) == Errc::NotConnected);
        break;
      }
      default: break;
    }
    CHECK(parsed);
    ++typed;
  });
  CHECK_EQ(typed, 13);
}

void testActorStates() {
  record()["actor_states"].forEach([](const Json& a) {
    auto s = decodeKitActorState(unhex(a["msgpack"].asStringView()));
    CHECK(s.has_value());
    const Json v = a["value"];
    CHECK_EQ(s->kind, v["kind"].asString());
    CHECK_EQ(s->id, v["id"].asInt64());
    CHECK(std::fabs(s->x - v["x"].asDouble()) < 1e-9 && std::fabs(s->z - v["z"].asDouble()) < 1e-9);
    CHECK(s->hp.has_value() == v["hp"].isNumber());
  });
  CHECK(!decodeKitActorState("not msgpack").has_value());
  CHECK(!decodeKitActorState(Json::parse(R"({"pose":1})").toMsgpack()).has_value());
}

// ---- the starters' scripts, replayed ----

void testWorld() {
  Replay gw(starter("world-tick"));
  auto p1 = gw.runtime("player", 1);
  auto p2 = gw.runtime("player", 2);
  auto dev = gw.runtime("developer", 9);
  WorldKit w1(p1), w2(p2), wd(dev);

  CHECK(gw.wait([&](KitCallback cb) { w1.conditions().start(cb); }).ok());
  CHECK(gw.wait([&](KitCallback cb) { w1.nodes().start(cb); }).ok());
  CHECK_EQ(w1.nodes().list("Node").size(), 2u);
  CHECK_EQ(gw.wait([&](KitCallback cb) { w1.state(cb); }).value()["nodes"].size(), 2u);
  CHECK_EQ(gw.wait([&](KitCallback cb) { w1.harvest(0, cb); }).value().asInt64(), 1);
  CHECK(gw.until([&] { return w1.nodes().member(0) && w1.nodes().member(0)->props["amount"].asInt64() == 1; }));
  // The script's resend of the same action: answered from the hub's receipt.
  KitCallOptions resent;
  resent.actionId = "resent";
  CHECK_EQ(gw.wait([&](KitCallback cb) { p1->call("world", "", "harvest", JVal::object({{"node", JVal(0)}}), cb, resent); }).value().asInt64(), 1);
  CHECK_EQ(gw.wait([&](KitCallback cb) { w1.harvest(0, cb); }).code(), "cooldown");
  CHECK_EQ(gw.wait([&](KitCallback cb) { w2.harvest(7, cb); }).code(), "not_found");
  gw.serverStep();
  CHECK(gw.until([&] { return w1.conditions().position().second == 1; }));
  CHECK(w1.conditions().get()["night"].isBool());
  CHECK(gw.wait([&](KitCallback cb) { w1.conditions().resync(cb); }).ok());
  CHECK(gw.wait([&](KitCallback cb) { w1.nodes().resync(cb); }).ok());
  Json schedule;
  starter("world-tick")["steps"].forEach([&](const Json& s) {
    if (s["call"].asStringView() == "schedule") schedule = s["args"];
  });
  JVal event = JVal::object({{"name", JVal("blood-moon")},
                             {"start_ms", JVal(schedule["start_ms"].asInt64())},
                             {"end_ms", JVal(schedule["end_ms"].asInt64())}});
  CHECK_EQ(gw.wait([&](KitCallback cb) { w1.schedule(event, cb); }).code(), "forbidden");
  event["modifiers"] = JVal::object({{"aggro", JVal(2)}});
  CHECK(gw.wait([&](KitCallback cb) { wd.schedule(event, cb); }).value().asBool());
  CHECK(gw.until([&] { return w1.conditions().get()["modifiers"]["aggro"].asDouble() == 2.0; }));
  gw.done();
}

void testMatchmaking() {
  Replay gw(starter("matchmaker"));
  std::vector<std::shared_ptr<KitRuntime>> rts;
  std::vector<std::unique_ptr<MatchmakingKit>> kits;
  for (int p = 1; p <= 5; ++p) {
    rts.push_back(gw.runtime("player", p));
    kits.push_back(std::make_unique<MatchmakingKit>(rts.back()));
  }
  auto q = [&](int p) -> MatchQueue& { return kits[p - 1]->queue("ranked"); };
  auto state = [](const KitReply& r) { return r.value()["state"].asString(); };

  CHECK(gw.wait([&](KitCallback cb) { q(1).counts().start(cb); }).ok());
  CHECK_EQ(q(1).counts().get()["waiting"].asInt64(), 0);
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(1).join(MatchJoin{1000, {}, {}}, cb); })), "queued");
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(2).join(MatchJoin{1020, {}, {}}, cb); })), "queued");
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(1).status(cb); })), "queued");
  gw.serverStep();
  CHECK(gw.until([&] { return q(1).counts().position().second == 1 && q(1).counts().get()["waiting"].asInt64() == 0; }));
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(1).status(cb); })), "proposed");
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(1).accept(cb); })), "proposed");
  std::string matched;
  q(2).onMatched([&](const std::string& s) { matched = s; });
  const KitReply done = gw.wait([&](KitCallback cb) { q(2).accept(cb); });
  CHECK_EQ(state(done), "matched");
  CHECK_EQ(done.value()["session"].asString(), "ranked-1-1");
  CHECK_EQ(matched, "ranked-1-1");
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(2).status(cb); })), "matched");
  CHECK_EQ(gw.wait([&](KitCallback cb) { q(3).accept(cb); }).code(), "not_found");
  const KitReply forming = gw.wait([&](KitCallback cb) { q(3).join(MatchJoin{900, {4}, {}}, cb); });
  CHECK_EQ(state(forming), "party");
  CHECK_EQ(forming.value()["members"].size(), 2u);
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(4).status(cb); })), "party");
  CHECK_EQ(gw.wait([&](KitCallback cb) { q(5).join(MatchJoin{900, {}, 3}, cb); }).code(), "forbidden");
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(4).join(MatchJoin{950, {}, 3}, cb); })), "queued");
  CHECK_EQ(q(4).place()["state"].asString(), "queued");
  CHECK_EQ(state(gw.wait([&](KitCallback cb) { q(4).status(cb); })), "queued");
  CHECK(gw.wait([&](KitCallback cb) { q(1).counts().resync(cb); }).ok());
  CHECK_EQ(q(1).counts().get()["waiting"].asInt64(), 2);
  gw.done();
}

void testSessions() {
  Replay gw(starter("session"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto r3 = gw.runtime("player", 3);
  GameSessionHandle s1(r1, "session", "ranked-1-1"), s2(r2, "session", "ranked-1-1"), s3(r3, "session", "ranked-1-1");

  CHECK(gw.wait([&](KitCallback cb) { s1.watch(cb); }).ok());
  CHECK_EQ(s1.state().get()["phase"].asString(), "lobby");
  gw.serverStep();  // the matchmaker reserves it
  CHECK(gw.until([&] { return s1.state().get()["reserved"].size() == 2; }));
  CHECK_EQ(gw.wait([&](KitCallback cb) { s3.join(cb); }).code(), "reserved");
  CHECK(gw.wait([&](KitCallback cb) { s1.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { s2.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { s1.ready(true, cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { s2.startGame(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { s2.ready(true, cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { s1.startGame(cb); }).value().asBool());
  CHECK(gw.until([&] { return s1.state().get()["phase"].asStringView() == "playing"; }));
  CHECK(s1.currentTurn() == std::optional<std::int64_t>(1));
  CHECK(s1.isMyTurn());
  CHECK_EQ(gw.wait([&](KitCallback cb) { s2.act(JVal::object({{"points", JVal(1)}}), cb); }).code(), "not_your_turn");
  CHECK(gw.wait([&](KitCallback cb) { s1.act(JVal::object({{"points", JVal(3)}}), cb); }).value().asBool());
  // The same move sent again (the reply was lost): the hub answers from its receipt.
  KitCallOptions resent;
  resent.actionId = "resent";
  CHECK(gw.wait([&](KitCallback cb) { r1->call("session", "ranked-1-1", "act", JVal::object({{"points", JVal(3)}}), cb, resent); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { s2.act(JVal::object({{"points", JVal(9)}}), cb); }).code(), "invalid_move");
  gw.serverStep();  // player 2's turn times out
  Json last;
  starter("session")["steps"].forEach([&](const Json& s) {
    s["published"].forEach([&](const Json& p) { last = p["value"]; });
  });
  CHECK(gw.until([&] { return s1.state().position().second == last["seq"].asInt64(); }));
  CHECK(same(s1.state().get(), bodyOf(last)));
  CHECK(sessionTurn(s1.state().get()) == std::optional<std::int64_t>(1));
  CHECK(gw.wait([&](KitCallback cb) { s1.state().resync(cb); }).ok());
  CHECK(same(s1.state().get(), bodyOf(last)));
  CHECK_EQ(gw.wait([&](KitCallback cb) { r1->call("session", "ranked-1-1", "state", JVal(), cb); }).value()["phase"].asString(), "playing");
  gw.done();
}

void testNpcs() {
  Replay gw(starter("npc-mobs"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  NpcsKit k1(r1), k2(r2);
  CHECK_EQ(k1.keyAt(1, 2), "4,4");
  NpcArea& area = k1.area("0,0");
  CHECK(gw.wait([&](KitCallback cb) { area.watch(cb); }).ok());
  CHECK_EQ(area.mobs().size(), 6u);
  CHECK_EQ(area.npcs().size(), 2u);
  gw.serverStep();
  CHECK(gw.until([&] { return area.members().position().second == 8; }));
  CHECK(gw.wait([&](KitCallback cb) { area.members().resync(cb); }).ok());
  CHECK_EQ(gw.wait([&](KitCallback cb) { area.attack(1, cb); }).code(), "out_of_reach");
  CHECK_EQ(gw.wait([&](KitCallback cb) { area.talk(7, cb); }).value()["name"].asString(), "Warden");
  const std::int64_t pet = gw.wait([&](KitCallback cb) { area.adopt("cat", "Mog", cb); }).value().asInt64();
  CHECK_EQ(pet, 9);
  CHECK(gw.wait([&](KitCallback cb) { area.summon(pet, cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k2.area("0,0").summon(pet, cb); }).code(), "forbidden");
  const Json mine = gw.wait([&](KitCallback cb) { area.myPets(cb); }).value();
  CHECK_EQ(mine.at(0)["name"].asString(), "Mog");
  CHECK_EQ(mine.at(0)["bond"].asInt64(), 1);
  CHECK_EQ(mine.at(0)["mode"].asString(), "follow");
  gw.serverStep();
  CHECK(gw.until([&] { return area.pets().size() == 1; }));
  CHECK(!area.pets()[0].props["bond"].ok());  // the bond is never published
  gw.done();
}

// ---- the genre starters ----

/// The first step of `script` that calls `method`.
Json stepCalling(const Json& script, std::string_view method) {
  Json found;
  script["steps"].forEach([&](const Json& s) {
    if (!found.ok() && s["call"].asStringView() == method) found = s;
  });
  CHECK(found.ok());
  return found;
}

bool has(const std::vector<std::string>& seen, std::string_view what) { return std::find(seen.begin(), seen.end(), what) != seen.end(); }

KitCallOptions resentAs(std::string id) {
  KitCallOptions o;
  o.actionId = std::move(id);
  return o;
}

void testLeaderboards() {
  Replay gw(starter("leaderboard"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto d9 = gw.runtime("developer", 9);
  LeaderboardsKit k1(r1), k2(r2), kd(d9);
  LeaderboardHandle& board = k1.board("weekly");
  CHECK(gw.wait([&](KitCallback cb) { board.watch(cb); }).ok());
  CHECK_EQ(board.view().get()["season"].asInt64(), 1);
  CHECK_EQ(gw.wait([&](KitCallback cb) { board.submit(1, 9, cb); }).code(), "forbidden");
  for (int i = 0; i < 4; ++i) gw.serverStep();  // the hubs that decided the scores submit them; the board publishes
  CHECK(gw.until([&] { return board.view().get()["top"].size() == 2; }));
  const Json top = gw.wait([&](KitCallback cb) { board.top(cb, {10, std::nullopt}); }).value();
  CHECK_EQ(top.at(0)["player"].asInt64(), 2);
  CHECK_EQ(top.at(1)["player"].asInt64(), 1);
  const Json me = gw.wait([&](KitCallback cb) { board.me(cb); }).value();
  CHECK_EQ(me["rank"].asInt64(), 2);
  CHECK_EQ(me["score"].asInt64(), 9);
  CHECK_EQ(gw.wait([&](KitCallback cb) { k2.board("weekly").around(cb, {std::nullopt, 1}); }).value().size(), 2u);
  CHECK_EQ(gw.wait([&](KitCallback cb) { board.season(cb); }).value()["entries"].asInt64(), 2);
  CHECK_EQ(gw.wait([&](KitCallback cb) { board.roll(cb); }).code(), "forbidden");
  CHECK_EQ(gw.wait([&](KitCallback cb) { kd.board("weekly").roll(cb); }).value().asInt64(), 2);
  CHECK(gw.wait([&](KitCallback cb) { board.view().resync(cb); }).ok());
  CHECK_EQ(board.view().get()["season"].asInt64(), 2);
  gw.done();
}

void testTables() {
  const Json script = starter("deck");
  Replay gw(script);
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  TablesKit k1(r1), k2(r2);
  CardTable& t1 = k1.table("t1");
  CardTable& t2 = k2.table("t1");
  CHECK(gw.wait([&](KitCallback cb) { t1.table().start(cb); }).ok());
  CHECK_EQ(t1.table().get()["phase"].asString(), "lobby");
  CHECK(gw.wait([&](KitCallback cb) { t1.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { t2.join(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { t2.start(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { t1.start(cb); }).value().asBool());
  const Json view = gw.wait([&](KitCallback cb) { r1->call("deck", "t1", "state", JVal(), cb); }).value();
  CHECK_EQ(view["phase"].asString(), "playing");
  const std::int64_t turn = view["turn"].asInt64();
  auto& mover = turn == 1 ? r1 : r2;
  auto& waiter = turn == 1 ? r2 : r1;
  CHECK_EQ(gw.wait([&](KitCallback cb) { mover->call("deck", "t1", "hand", JVal(), cb); }).value()["cards"].size(), 5u);
  CHECK_EQ(gw.wait([&](KitCallback cb) { waiter->call("deck", "t1", "hand", JVal(), cb); }).value()["cards"].size(), 5u);
  CardTable& moving = turn == 1 ? t1 : t2;
  CardTable& waiting = turn == 1 ? t2 : t1;
  CHECK_EQ(gw.wait([&](KitCallback cb) { waiting.draw(cb); }).code(), "not_your_turn");
  Json play;
  script["steps"].forEach([&](const Json& s) {
    if (!play.ok() && (s["call"].asStringView() == "play" || (s["call"].asStringView() == "draw" && s["ok"].asBool()))) play = s;
  });
  CHECK(play.ok());
  if (play["call"].asStringView() == "play") {
    const std::string card = play["args"]["card"].asString();
    const std::optional<std::string> suit = play["args"]["suit"].isString() ? std::optional(play["args"]["suit"].asString()) : std::nullopt;
    CHECK(gw.wait([&](KitCallback cb) { moving.play(card, suit, cb); }).value().asBool());
    JVal again = JVal::object({{"card", JVal(card)}});
    if (suit) again["suit"] = JVal(*suit);
    CHECK(gw.wait([&](KitCallback cb) { mover->call("deck", "t1", "play", again, cb, resentAs("resent")); }).value().asBool());
  } else {
    CHECK(gw.wait([&](KitCallback cb) { moving.draw(cb); }).value().asBool());
    CHECK(gw.wait([&](KitCallback cb) { moving.pass(cb); }).value().asBool());
  }
  CHECK(gw.wait([&](KitCallback cb) { t1.table().resync(cb); }).ok());
  CHECK(t1.table().get()["hand_counts"].ok());
  gw.done();
}

void testTactics() {
  const Json script = starter("tactics");
  Replay gw(script);
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  TacticsKit k1(r1), k2(r2);
  TacticsBattle& b1 = k1.battle("b1");
  TacticsBattle& b2 = k2.battle("b1");
  CHECK(gw.wait([&](KitCallback cb) { b1.watch(cb); }).ok());
  CHECK_EQ(b1.battle().get()["phase"].asString(), "lobby");
  CHECK(gw.wait([&](KitCallback cb) { b1.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { b2.join(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { b2.start(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { b1.start(cb); }).value().asBool());
  CHECK(gw.until([&] { return b1.battle().get()["phase"].asStringView() == "players"; }));
  CHECK(gw.wait([&](KitCallback cb) { r1->call("tactics", "b1", "state", JVal(), cb); }).value()["units"].size() > 0);
  const auto mine = b1.myUnits();
  CHECK(!mine.empty());
  const Json move = stepCalling(script, "move")["args"];
  CHECK_EQ(move["x"].asInt64(), mine.at(0)["x"].asInt64() + 1);
  CHECK_EQ(move["z"].asInt64(), mine.at(0)["z"].asInt64());
  CHECK(gw.wait([&](KitCallback cb) { b1.move(move["x"].asInt64(), move["z"].asInt64(), cb); }).value().asBool());
  const JVal again = JVal::object({{"x", JVal(move["x"].asInt64())}, {"z", JVal(move["z"].asInt64())}});
  CHECK(gw.wait([&](KitCallback cb) { r1->call("tactics", "b1", "move", again, cb, resentAs("resent")); }).value().asBool());
  const std::int64_t target = stepCalling(script, "attack")["args"]["target"].asInt64();
  CHECK_EQ(gw.wait([&](KitCallback cb) { b1.attack(target, cb); }).code(), "out_of_reach");
  CHECK(gw.wait([&](KitCallback cb) { b1.endTurn(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { b2.endTurn(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { b1.battle().resync(cb); }).ok());
  CHECK(b1.battle().get()["log"].size() > 0);
  gw.done();
}

void testEncounters() {
  Replay gw(starter("director"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  EncountersKit k1(r1), k2(r2);
  Encounter& e = k1.encounter("gate");
  std::vector<std::string> events;
  e.view().onEvent([&](const Json& body, const KitHeader&) { events.push_back(body["type"].asString()); });
  CHECK(gw.wait([&](KitCallback cb) { e.watch(cb); }).ok());
  CHECK_EQ(e.view().get()["phase"].asString(), "lobby");
  CHECK(gw.wait([&](KitCallback cb) { e.join(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k2.encounter("gate").start(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { e.start(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { e.build(5, 3, cb); }).value().asInt64(), 20);
  const JVal again = JVal::object({{"x", JVal(5)}, {"z", JVal(3)}});
  CHECK_EQ(gw.wait([&](KitCallback cb) { r1->call("director", "gate", "build", again, cb, resentAs("resent")); }).value().asInt64(), 20);
  CHECK_EQ(gw.wait([&](KitCallback cb) { e.build(99, 0, cb); }).code(), "bad_request");
  gw.serverStep();  // the first wave
  CHECK(gw.until([&] { return has(events, "wave_started"); }));
  const Json state = gw.wait([&](KitCallback cb) { r1->call("director", "gate", "state", JVal(), cb); }).value();
  const std::int64_t enemy = state["enemies"].at(0)["id"].asInt64();
  CHECK_EQ(gw.wait([&](KitCallback cb) { e.hit(enemy, cb); }).value().asInt64(), 15);
  CHECK_EQ(gw.wait([&](KitCallback cb) { e.hit(enemy, cb); }).code(), "cooldown");
  CHECK(gw.wait([&](KitCallback cb) { e.view().resync(cb); }).ok());
  CHECK_EQ(e.view().get()["enemies"].at(0)["hp"].asInt64(), 15);
  gw.done();
}

void testMinigames() {
  Replay gw(starter("minigame"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto r3 = gw.runtime("player", 3);
  MinigamesKit k1(r1), k2(r2), k3(r3);
  Duel& d1 = k1.duel("t1");
  Duel& d2 = k2.duel("t1");
  CHECK(gw.wait([&](KitCallback cb) { d1.watch(cb); }).ok());
  CHECK_EQ(d1.view().get()["phase"].asString(), "waiting");
  CHECK(!gw.wait([&](KitCallback cb) { d1.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { d2.join(cb); }).value().asBool());  // the second seat starts the first round
  CHECK_EQ(gw.wait([&](KitCallback cb) { k3.duel("t1").join(cb); }).code(), "full");
  CHECK(!gw.wait([&](KitCallback cb) { d1.throwHand("rock", cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { d1.throwHand("paper", cb); }).code(), "conflict");
  CHECK_EQ(gw.wait([&](KitCallback cb) { d2.throwHand("lizard", cb); }).code(), "bad_request");
  CHECK(gw.wait([&](KitCallback cb) { d2.throwHand("scissors", cb); }).value().asBool());
  CHECK(gw.until([&] { return d1.view().get()["last"]["round"].asInt64() == 1; }));
  CHECK_EQ(d1.view().get()["last"]["hands"].at(0).asString(), "rock");
  CHECK_EQ(d1.view().get()["last"]["hands"].at(1).asString(), "scissors");
  CHECK(!gw.wait([&](KitCallback cb) { d1.throwHand("paper", cb); }).value().asBool());
  const JVal again = JVal::object({{"hand", JVal("paper")}});
  CHECK(!gw.wait([&](KitCallback cb) { r1->call("minigame", "t1", "throw", again, cb, resentAs("resent")); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { d2.throwHand("rock", cb); }).value().asBool());
  CHECK(gw.until([&] { return d1.view().get()["winner"].isNumber() && d1.view().get()["winner"].asInt64() == 1; }));
  CHECK(gw.wait([&](KitCallback cb) { d2.rematch(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { d1.view().resync(cb); }).ok());
  CHECK_EQ(d1.view().get()["phase"].asString(), "playing");
  gw.done();
}

void testInstances() {
  Replay gw(starter("instance"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto r3 = gw.runtime("player", 3);
  InstancesKit k1(r1), k2(r2), k3(r3);
  InstanceRun& run1 = k1.run("run-a");
  InstanceRun& run2 = k2.run("run-a");
  CHECK(gw.wait([&](KitCallback cb) { run1.watch(cb); }).ok());
  CHECK_EQ(run1.view().get()["phase"].asString(), "unopened");
  CHECK_EQ(gw.wait([&](KitCallback cb) { run1.open({7, {}}, cb); }).value().asInt64(), 7);
  CHECK_EQ(gw.wait([&](KitCallback cb) { run2.join(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { run1.invite(2, cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { run2.join(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k3.run("run-a").enter(cb); }).code(), "not_member");
  const std::string first = gw.wait([&](KitCallback cb) { run2.enter(cb); }).value().asString();
  CHECK(first.find("won") != std::string::npos);
  CHECK_EQ(gw.wait([&](KitCallback cb) { r2->call("instance", "run-a", "enter", JVal(), cb, resentAs("resent")); }).value().asString(), first);
  CHECK(gw.wait([&](KitCallback cb) { run1.enter(cb); }).value().asString().find("the boss: won") != std::string::npos);
  CHECK_EQ(gw.wait([&](KitCallback cb) { run1.enter(cb); }).code(), "finished");
  CHECK(gw.wait([&](KitCallback cb) { run1.view().resync(cb); }).ok());
  CHECK_EQ(run1.view().get()["phase"].asString(), "cleared");
  gw.done();
}

void testArenas() {
  Replay gw(starter("arena"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto r3 = gw.runtime("player", 3);
  ArenasKit k1(r1), k2(r2), k3(r3);
  ArenaMatch& a1 = k1.arena("pit");
  ArenaMatch& a2 = k2.arena("pit");
  std::vector<std::string> events;
  a1.view().onEvent([&](const Json& body, const KitHeader&) { events.push_back(body["type"].asString()); });
  CHECK(gw.wait([&](KitCallback cb) { a1.watch(cb); }).ok());
  CHECK_EQ(a1.view().get()["phase"].asString(), "waiting");
  CHECK(gw.wait([&](KitCallback cb) { a1.join("warrior", cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { a2.join("mage", cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k3.arena("pit").join("bard", cb); }).code(), "bad_request");
  CHECK_EQ(gw.wait([&](KitCallback cb) { a2.start(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { a1.start(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { a1.cast("strike", 11, 10, cb); }).value().asBool());
  const JVal again = JVal::object({{"ability", JVal("strike")}, {"x", JVal(11)}, {"z", JVal(10)}});
  CHECK(gw.wait([&](KitCallback cb) { r1->call("arena", "pit", "cast", again, cb, resentAs("resent")); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { a1.cast("strike", 11, 10, cb); }).code(), "cooldown");
  CHECK(!gw.wait([&](KitCallback cb) { a2.cast("bolt", 10, 10, cb); }).value().asBool());  // a projectile lands later
  gw.serverStep();
  CHECK(gw.until([&] { return has(events, "impact"); }));
  const Json kept = gw.wait([&](KitCallback cb) { a2.move(30, 10, cb); }).value();  // a jump is refused
  CHECK_EQ(kept.at(0).asDouble(), 11.0);
  CHECK_EQ(kept.at(1).asDouble(), 10.0);
  CHECK(gw.wait([&](KitCallback cb) { a1.view().resync(cb); }).ok());
  CHECK_EQ(a1.view().get()["fighters"].size(), 2u);
  gw.done();
}

void testRace() {
  Replay gw(starter("racing", "race"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  RacingKit k1(r1), k2(r2);
  Race& race = k1.race("r1");
  std::vector<std::string> events;
  race.view().onEvent([&](const Json& body, const KitHeader&) {
    events.push_back(body["type"].asStringView() == "timing" ? body["stage"].asString() : body["type"].asString());
  });
  CHECK(gw.wait([&](KitCallback cb) { race.watch(cb); }).ok());
  CHECK_EQ(race.view().get()["phase"].asString(), "lobby");
  CHECK(gw.wait([&](KitCallback cb) { race.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { k2.race("r1").join(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k2.race("r1").start(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { race.start(cb); }).value().asBool());
  gw.serverStep();  // the countdown ends
  CHECK(gw.until([&] { return has(events, "started"); }));
  for (const double x : {5.0, 10.0, 5.0, 0.0}) {
    gw.serverStep();
    const Json kept = gw.wait([&](KitCallback cb) { race.pos(x, 0, cb); }).value();
    CHECK_EQ(kept.at(0).asDouble(), x);
  }
  CHECK(gw.until([&] { return has(events, "finished"); }));
  const Json ghost = gw.wait([&](KitCallback cb) { k2.race("r1").ghost(cb); }).value();
  CHECK_EQ(ghost["player"].asInt64(), 1);
  CHECK(ghost["frames"].size() >= 4);
  gw.serverStep();  // the rest's time runs out
  CHECK(gw.wait([&](KitCallback cb) { race.view().resync(cb); }).ok());
  const Json done = race.view().get();
  CHECK_EQ(done["phase"].asString(), "finished");
  bool secondOut = false;
  done["racers"].forEach([&](const Json& r) {
    if (r["player"].asInt64() == 2) secondOut = r["did_not_finish"].asBool();
  });
  CHECK(secondOut);
  gw.done();
}

void testBall() {
  Replay gw(starter("racing", "ball"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  RacingKit k1(r1), k2(r2);
  BallMatch& m1 = k1.ball("m1");
  BallMatch& m2 = k2.ball("m1");
  std::vector<std::string> events;
  m1.view().onEvent([&](const Json& body, const KitHeader&) { events.push_back(body["type"].asString()); });
  CHECK(gw.wait([&](KitCallback cb) { m1.watch(cb); }).ok());
  CHECK_EQ(m1.view().get()["phase"].asString(), "lobby");
  CHECK(gw.wait([&](KitCallback cb) { m1.join("red", cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { m2.join("blue", cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { m1.start(cb); }).value().asBool());
  for (const double x : {22.0, 29.5}) {
    gw.serverStep();
    CHECK_EQ(gw.wait([&](KitCallback cb) { m1.pos(x, 20, cb); }).value().at(0).asDouble(), x);
  }
  CHECK_EQ(gw.wait([&](KitCallback cb) { m2.claim(cb); }).code(), "out_of_reach");
  CHECK(gw.wait([&](KitCallback cb) { m1.claim(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { m1.shoot(60, 20, cb); }).value().asBool());
  gw.serverStep();
  CHECK(gw.until([&] { return has(events, "goal"); }));
  CHECK(gw.wait([&](KitCallback cb) { m1.view().resync(cb); }).ok());
  CHECK_EQ(m1.view().get()["score"].at(0).asInt64(), 1);
  CHECK_EQ(m1.view().get()["score"].at(1).asInt64(), 0);
  gw.done();
}

/// The point `id` of a territory view.
Json pointOf(const Json& view, std::string_view id) {
  Json found;
  view["points"].forEach([&](const Json& p) {
    if (p["id"].asStringView() == id) found = p;
  });
  return found;
}

void testTerritory() {
  Replay gw(starter("territory", "territory"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto d9 = gw.runtime("developer", 9);
  TerritoryKit k1(r1), k2(r2), kd(d9);
  TerritoryMap& m = k1.map("valley");
  std::vector<std::string> events;
  m.view().onEvent([&](const Json& body, const KitHeader&) { events.push_back(body["type"].asString()); });
  CHECK(gw.wait([&](KitCallback cb) { m.watch(cb); }).ok());
  CHECK_EQ(m.view().get()["phase"].asString(), "open");
  CHECK(gw.wait([&](KitCallback cb) { m.join("blue", cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { m.join("red", cb); }).code(), "conflict");
  CHECK_EQ(gw.wait([&](KitCallback cb) { k2.map("valley").join("green", cb); }).code(), "bad_request");
  gw.serverStep();  // two scans
  CHECK(gw.until([&] { return has(events, "captured"); }));
  const Json state = gw.wait([&](KitCallback cb) { r1->call("territory", "valley", "state", JVal(), cb); }).value();
  CHECK_EQ(pointOf(state, "tower")["owner"].asString(), "blue");
  CHECK_EQ(gw.wait([&](KitCallback cb) { m.reset(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { kd.map("valley").reset(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { m.view().resync(cb); }).ok());
  CHECK(pointOf(m.view().get(), "tower")["owner"].isNull());
  gw.done();
}

void testZone() {
  Replay gw(starter("territory", "zone"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto r3 = gw.runtime("player", 3);
  TerritoryKit k1(r1), k2(r2), k3(r3);
  ZoneMatch& z1 = k1.zone("match-1");
  CHECK(gw.wait([&](KitCallback cb) { z1.watch(cb); }).ok());
  CHECK_EQ(z1.view().get()["phase"].asString(), "lobby");
  CHECK(gw.wait([&](KitCallback cb) { z1.join(cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { k2.zone("match-1").join(cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k2.zone("match-1").start(cb); }).code(), "forbidden");
  CHECK(gw.wait([&](KitCallback cb) { z1.start(cb); }).value().asBool());
  gw.serverStep();  // a scan
  CHECK(gw.until([&] {
    bool outside = false;
    z1.view().get()["players"].forEach([&](const Json& c) { outside = outside || c["outside"].asBool(); });
    return outside;
  }));
  const Json state = gw.wait([&](KitCallback cb) { r1->call("zone", "match-1", "state", JVal(), cb); }).value();
  CHECK(!state["players"].at(0)["outside"].asBool());
  CHECK(state["players"].at(1)["outside"].asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { k3.zone("match-1").join(cb); }).code(), "started");
  CHECK(gw.wait([&](KitCallback cb) { z1.view().resync(cb); }).ok());
  CHECK_EQ(z1.view().get()["alive"].asInt64(), 2);
  gw.done();
}

// ---- the economy ----

JVal items(const char* item, std::int64_t n) { return JVal::object({{"items", JVal::object({{item, JVal(n)}})}}); }
JVal coins(const char* currency, std::int64_t n) { return JVal::object({{"currency", JVal::object({{currency, JVal(n)}})}}); }

void testPlayer() {
  Replay gw(starter("player"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto d9 = gw.runtime("developer", 9);
  PlayerKit p1(r1), p2(r2);
  InventoryKit inventory(p1);
  WalletKit wallet(p1);
  ProgressionKit progression(p1), progression2(p2);
  AchievementsKit achievements(p1);
  QuestsKit quests(p1);
  CraftingKit crafting(p1);
  LootKit loot(p1);
  ProductionKit production(p1);
  std::vector<std::string> events;
  p1.onEvent([&](const Json& e) { events.push_back(e["type"].asString()); });
  CHECK(gw.wait([&](KitCallback cb) { p1.watch(cb, {"profile"}); }).ok());
  CHECK_EQ(progression.profile().get()["level"].asInt64(), 1);
  const Json me = gw.wait([&](KitCallback cb) { p1.me(cb); }).value();
  CHECK_EQ(me["wallet"]["balances"]["gold"].asInt64(), 100);
  CHECK_EQ(gw.wait([&](KitCallback cb) { r2->call("player", "1", "me", JVal(), cb); }).code(), "forbidden");
  CHECK_EQ(gw.wait([&](KitCallback cb) { progression2.profileOf(1, cb); }).value()["player"].asInt64(), 1);
  CHECK(gw.wait([&](KitCallback cb) { crafting.recipes(cb); }).value().size() >= 1);
  const JVal deposit = JVal::object({{"ref", JVal("seed-wood")}, {"bundle", items("wood", 4)}});
  CHECK(gw.wait([&](KitCallback cb) { d9->call("player", "1", "deposit", deposit, cb); }).value().asBool());
  const Json crafted = gw.wait([&](KitCallback cb) { crafting.craft("plank", 2, cb); }).value();
  CHECK_EQ(crafted["made"]["items"]["plank"].asInt64(), 2);
  CHECK_EQ(crafted["earned"]["achievements"].at(0).asString(), "first_craft");
  const JVal again = JVal::object({{"recipe", JVal("plank")}, {"times", JVal(2)}});
  CHECK(same(gw.wait([&](KitCallback cb) { r1->call("player", "1", "craft", again, cb, resentAs("resent")); }).value(), crafted));
  CHECK_EQ(gw.wait([&](KitCallback cb) { crafting.craft("plank", std::nullopt, cb); }).code(), "insufficient");
  CHECK_EQ(gw.wait([&](KitCallback cb) { quests.step("crafted", cb); }).code(), "forbidden");
  CHECK_EQ(gw.wait([&](KitCallback cb) { quests.step("tutorial:inventory", cb); }).value()["quests"].at(0).asString(), "welcome");
  CHECK_EQ(gw.wait([&](KitCallback cb) { quests.claim("welcome", cb); }).value()["currency"]["gold"].asInt64(), 25);
  CHECK_EQ(gw.wait([&](KitCallback cb) { quests.claim("welcome", cb); }).code(), "not_ready");
  CHECK(gw.wait([&](KitCallback cb) { loot.open("chest", cb); }).value()["roll"]["count"].asInt64() > 0);
  gw.serverStep();  // half an hour away
  const Json collected = gw.wait([&](KitCallback cb) { production.collect(cb); }).value();
  CHECK_EQ(collected["items"]["wood"].asInt64(), 60);
  CHECK_EQ(collected["items"]["ore"].asInt64(), 30);
  for (int i = 0; i < 4; ++i) gw.serverStep();  // a market holds, settles, releases and sells
  const JVal grant = JVal::object({{"ref", JVal("nope")}, {"bundle", coins("gold", 1)}});
  CHECK_EQ(gw.wait([&](KitCallback cb) { r1->call("player", "1", "grant", grant, cb); }).code(), "forbidden");
  gw.serverStep();  // a run's reward
  CHECK(gw.until([&] { return has(events, "level"); }));
  CHECK(gw.wait([&](KitCallback cb) { inventory.takeMail(cb); }).ok());
  CHECK(gw.wait([&](KitCallback cb) { wallet.watch(cb); }).ok());
  CHECK_EQ(wallet.balance("gold"), 80);
  CHECK(gw.wait([&](KitCallback cb) { inventory.watch(cb); }).ok());
  CHECK_EQ(inventory.count("plank"), 2);
  CHECK(gw.wait([&](KitCallback cb) { progression.watch(cb); }).ok());
  CHECK_EQ(progression.progress().get()["level"].asInt64(), 4);
  bool unlocked = false;
  for (const auto& a : achievements.list()) unlocked = unlocked || (a["id"].asStringView() == "first_craft" && a["unlocked_at"].isNumber());
  CHECK(unlocked);
  bool welcomeDone = false;
  for (const auto& q : quests.list()) welcomeDone = welcomeDone || (q["id"].asStringView() == "welcome" && q["state"].asStringView() == "done");
  CHECK(welcomeDone);
  gw.serverStep();
  CHECK(gw.until([&] { return progression.profile().get()["level"].asInt64() == 4; }));
  const JVal topic = JVal::object({{"topic", JVal("player")}});
  CHECK_EQ(gw.wait([&](KitCallback cb) { r1->call("player", "1", "snapshot", topic, cb); }).value()["level"].asInt64(), 4);
  gw.done();
}

void testMarket() {
  Replay gw(starter("market"));
  auto r1 = gw.runtime("player", 1);
  auto r2 = gw.runtime("player", 2);
  auto r3 = gw.runtime("player", 3);
  MarketCores c1(r1, "market"), c2(r2, "market"), c3(r3, "market");
  ShopsKit s1(c1), s3(c3);
  MarketsKit m1(c1), m2(c2);
  TradesKit t1(c1), t2(c2), t3(c3);
  ShopHandle shop = s1.at("town");
  CHECK(gw.wait([&](KitCallback cb) { shop.watch(cb); }).ok());
  CHECK_EQ(shop.view().get()["currency"].asString(), "gold");
  CHECK_EQ(gw.wait([&](KitCallback cb) { shop.vendors(cb); }).value().at(0)["id"].asString(), "smith");
  CHECK_EQ(gw.wait([&](KitCallback cb) { shop.buy("smith", "sword", std::nullopt, cb); }).value()["items"]["sword"].asInt64(), 1);
  const JVal sword = JVal::object({{"vendor", JVal("smith")}, {"item", JVal("sword")}});
  CHECK(gw.wait([&](KitCallback cb) { r1->call("market", "town", "buy", sword, cb, resentAs("resent")); }).ok());
  CHECK_EQ(gw.wait([&](KitCallback cb) { shop.buy("smith", "sword", std::nullopt, cb); }).code(), "limit");
  CHECK_EQ(gw.wait([&](KitCallback cb) { s3.at("town").buy("smith", "potion", 2, cb); }).code(), "insufficient");
  CHECK_EQ(gw.wait([&](KitCallback cb) { shop.sell("smith", "sword", std::nullopt, cb); }).value()["currency"]["gold"].asInt64(), 20);
  CHECK_EQ(gw.wait([&](KitCallback cb) { m2.at("town").order({"ore", "sell", 4, 10}, cb); }).value()["open"].asInt64(), 10);
  const Json bought = gw.wait([&](KitCallback cb) { m1.at("town").order({"ore", "buy", 5, 6}, cb); }).value();
  CHECK_EQ(bought["open"].asInt64(), 0);
  CHECK_EQ(bought["fills"].at(0).at(0).asInt64(), 4);
  CHECK_EQ(gw.wait([&](KitCallback cb) { m1.at("town").book("ore", cb); }).value()["asks"].at(0).at(1).asInt64(), 4);
  const Json mine = gw.wait([&](KitCallback cb) { r2->call("market", "town", "orders", JVal(), cb); }).value();
  CHECK(gw.wait([&](KitCallback cb) { m2.at("town").cancel(mine.at(0)["id"].asInt64(), cb); }).value().asBool());
  TradeDesk desk = t1.at("town");
  const std::int64_t trade = gw.wait([&](KitCallback cb) { desk.open(2, cb); }).value().asInt64();
  CHECK(gw.wait([&](KitCallback cb) { desk.offer(trade, coins("gold", 30), cb); }).value().asBool());
  CHECK(gw.wait([&](KitCallback cb) { t2.at("town").offer(trade, items("ore", 2), cb); }).value().asBool());
  CHECK_EQ(gw.wait([&](KitCallback cb) { desk.accept(trade, cb); }).value().asString(), "open");
  CHECK_EQ(gw.wait([&](KitCallback cb) { t2.at("town").accept(trade, cb); }).value().asString(), "settling");
  CHECK_EQ(gw.wait([&](KitCallback cb) { r1->call("market", "town", "trades", JVal(), cb); }).value().size(), 0u);
  CHECK_EQ(gw.wait([&](KitCallback cb) { t3.at("town").decline(trade, cb); }).code(), "forbidden");
  gw.serverStep();
  CHECK(gw.wait([&](KitCallback cb) { shop.view().resync(cb); }).ok());
  CHECK_EQ(shop.view().get()["open_trades"].asInt64(), 0);
  gw.done();
}

// ---- the runtime ----

/// A gateway a test scripts call by call: `answer` gives each call's (status, payload).
struct Scripted {
  std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
  std::shared_ptr<graphql::Dispatcher> dispatcher = std::make_shared<graphql::Dispatcher>();
  std::function<std::pair<std::uint8_t, std::string>(const Frame&)> answer;
  std::vector<Frame> calls;
  std::size_t opened = 0, seen = 0;

  std::shared_ptr<KitRuntime> runtime(KitRuntimeOptions options = {}) {
    domains::ExecConnectOptions o;
    o.callTimeoutMs = 3000;
    options.connection = domains::ExecConnection::open(transport, dispatcher, domains::ExecEndpoint{"ws://gw.example", "player-1", "h1"}, o);
    options.playerId = "1";
    return std::make_shared<KitRuntime>("77", nullptr, dispatcher, options);
  }

  void pump() {
    auto conns = transport->all();
    for (; opened < conns.size(); ++opened) conns[opened]->open();
    if (conns.empty()) return;
    auto frames = conns[0]->frames();
    for (; seen < frames.size(); ++seen) {
      Frame f = parse(frames[seen]);
      if (f.tag == 0x01) {
        calls.push_back(f);
        auto [status, payload] = answer(f);
        conns[0]->deliver(reply(f.rid, status, payload));
      } else if (f.tag == 0x02 || f.tag == 0x03) {
        conns[0]->deliver(reply(f.rid, 0, ""));
      }
    }
  }

  template <typename Fn>
  bool until(Fn&& done, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      pump();
      dispatcher->drain();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  }
};

void testResends() {
  Scripted gw;
  int n = 0;
  gw.answer = [&](const Frame& f) -> std::pair<std::uint8_t, std::string> {
    ++n;
    if (f.name == "busy" && n == 1) return {2, "rate limited: a player may make 120 calls per 10000 ms; retry in 30 ms"};
    if (f.name == "lost" && n <= 2) return {8, "host went away"};
    if (f.name == "plain") return {8, "host went away"};
    if (f.name == "buy" && n == 1) return {1, "kit:unavailable: the player's hub did not answer; send it again"};
    if (f.name == "buy-plain") return {1, "kit:unavailable: no answer"};
    return {0, Json::parse("7").toMsgpack()};
  };
  KitRuntimeOptions options;
  options.retries = 2;
  auto rt = gw.runtime(options);

  std::optional<KitReply> busy;
  const auto before = std::chrono::steady_clock::now();
  rt->call("arena", "m1", "busy", JVal(), [&](KitReply r) { busy = r; });
  CHECK(gw.until([&] { return busy.has_value(); }));
  CHECK(busy->ok());
  CHECK_EQ(gw.calls.size(), 2u);
  CHECK(std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(25));  // it held as told

  // An unknown outcome is sent again only with an action id, which the hub answers once.
  n = 0;
  gw.calls.clear();
  std::optional<KitReply> lost;
  KitCallOptions action;
  action.action = true;
  rt->call("arena", "m1", "lost", JVal::object({{"x", JVal(1)}}), [&](KitReply r) { lost = r; }, action);
  CHECK(gw.until([&] { return lost.has_value(); }));
  CHECK(lost->ok());
  CHECK_EQ(gw.calls.size(), 3u);
  const Json first = Json::fromMsgpack(gw.calls[0].payload), third = Json::fromMsgpack(gw.calls[2].payload);
  CHECK(!first["action_id"].asString().empty());
  CHECK_EQ(first["action_id"].asString(), third["action_id"].asString());

  n = 0;
  gw.calls.clear();
  std::optional<KitReply> plain;
  rt->call("arena", "m1", "plain", JVal(), [&](KitReply r) { plain = r; });
  CHECK(gw.until([&] { return plain.has_value(); }));
  CHECK(plain->status == ExecStatus::Unavailable);
  CHECK_EQ(gw.calls.size(), 1u);

  // A hub that could not hear from another refuses `unavailable` and keeps no receipt: a call
  // with an action id is sent again with it, one without is the caller's to resend.
  n = 0;
  gw.calls.clear();
  std::optional<KitReply> buy;
  rt->call("market", "town", "buy", JVal::object({{"item", JVal("potion")}}), [&](KitReply r) { buy = r; }, action);
  CHECK(gw.until([&] { return buy.has_value(); }));
  CHECK(buy->ok());
  CHECK_EQ(gw.calls.size(), 2u);
  CHECK_EQ(Json::fromMsgpack(gw.calls[0].payload)["action_id"].asString(), Json::fromMsgpack(gw.calls[1].payload)["action_id"].asString());
  n = 0;
  gw.calls.clear();
  std::optional<KitReply> once;
  rt->call("market", "town", "buy-plain", JVal(), [&](KitReply r) { once = r; });
  CHECK(gw.until([&] { return once.has_value(); }));
  CHECK_EQ(once->code(), "unavailable");
  CHECK_EQ(gw.calls.size(), 1u);
}

void testPacing() {
  Scripted gw;
  gw.answer = [](const Frame&) -> std::pair<std::uint8_t, std::string> { return {0, Json::parse("true").toMsgpack()}; };
  KitRuntimeOptions options;
  options.callBudget = 5;
  options.budgetWindowMs = 200;
  auto rt = gw.runtime(options);
  int answered = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; ++i) rt->call("arena", "m1", "ping", JVal(), [&](KitReply r) { answered += r.ok(); });
  CHECK(gw.until([&] { return answered == 10; }));
  // Five went at once, the other five as the budget refilled (40 ms a token).
  CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(150));
}

int countTag(const std::shared_ptr<FakeConn>& c, std::uint8_t tag) {
  int n = 0;
  for (const auto& b : c->frames()) {
    if (!b.empty() && static_cast<std::uint8_t>(b[0]) == tag) ++n;
  }
  return n;
}

/// A runtime on a connection the test keeps, so close() must not close the socket.
struct Held {
  Scripted gw;
  std::shared_ptr<domains::ExecConnection> conn;
  std::shared_ptr<KitRuntime> rt;
};

Held hold(KitRuntimeOptions options) {
  Held h;
  h.gw.answer = [](const Frame&) -> std::pair<std::uint8_t, std::string> { return {0, Json::parse("true").toMsgpack()}; };
  domains::ExecConnectOptions o;
  o.callTimeoutMs = 3000;
  h.conn = domains::ExecConnection::open(h.gw.transport, h.gw.dispatcher, domains::ExecEndpoint{"ws://gw.example", "player-1", "h1"}, o);
  options.connection = h.conn;
  options.playerId = "1";
  h.rt = std::make_shared<KitRuntime>("77", nullptr, h.gw.dispatcher, options);
  return h;
}

void testCloseSettlesQueuedWork() {
  KitRuntimeOptions options;
  options.callBudget = 2;
  options.budgetWindowMs = 60000;
  Held h = hold(options);
  int pushes = 0;
  std::optional<KitReply> subbed;
  const auto handle = h.rt->subscribe("world", "", "conditions", [&](const domains::ExecPush&) { ++pushes; }, [&](KitReply r) { subbed = std::move(r); });
  CHECK(handle != 0);
  CHECK(h.gw.until([&] { return subbed.has_value(); }));
  CHECK(subbed->ok());

  int secondN = 0;
  std::optional<KitReply> first, second;
  h.rt->call("world", "", "harvest", JVal(), [&](KitReply r) { first = std::move(r); });
  h.rt->call("world", "", "harvest", JVal(), [&](KitReply r) {
    ++secondN;
    second = std::move(r);
  });
  CHECK(h.gw.until([&] { return first.has_value(); }));
  CHECK(!second.has_value());
  auto wire = h.gw.transport->all()[0];
  CHECK_EQ(countTag(wire, 0x01), 1);
  CHECK_EQ(countTag(wire, 0x03), 0);

  h.rt->unsubscribe(handle);
  CHECK_EQ(countTag(wire, 0x03), 0);

  h.rt->close();
  h.rt->close();
  CHECK(h.gw.until([&] { return second.has_value(); }));
  CHECK_EQ(secondN, 1);
  CHECK(second->status == ExecStatus::Unavailable);
  CHECK_EQ(second->message(), "the kit is closed");
  CHECK_EQ(countTag(wire, 0x03), 1);
  CHECK(!h.rt->connection());

  wire->deliver(push("world", "", "conditions", Json::parse("1").toMsgpack()));
  h.gw.dispatcher->drain();
  CHECK_EQ(pushes, 0);

  std::optional<KitReply> later;
  int laterN = 0;
  h.rt->call("world", "", "harvest", JVal(), [&](KitReply r) {
    ++laterN;
    later = std::move(r);
  });
  CHECK(h.gw.until([&] { return later.has_value(); }));
  CHECK_EQ(laterN, 1);
  CHECK_EQ(later->message(), "the kit is closed");
  CHECK_EQ(countTag(wire, 0x01), 1);

  h.rt.reset();
  wire->deliver(push("world", "", "conditions", Json::parse("2").toMsgpack()));
  h.gw.dispatcher->drain();
  CHECK_EQ(pushes, 0);
}

void testCloseFlushesPacedUnsubscribe() {
  KitRuntimeOptions options;
  options.callBudget = 1;
  options.budgetWindowMs = 60000;
  Held h = hold(options);
  std::optional<KitReply> subbed;
  const auto handle = h.rt->subscribe("world", "", "conditions", [](const domains::ExecPush&) {}, [&](KitReply r) { subbed = std::move(r); });
  CHECK(h.gw.until([&] { return subbed.has_value(); }));
  auto wire = h.gw.transport->all()[0];
  h.rt->unsubscribe(handle);
  CHECK_EQ(countTag(wire, 0x03), 0);
  h.rt->close();
  h.gw.dispatcher->drain();
  CHECK_EQ(countTag(wire, 0x03), 1);
}

void testCloseFailsQueuedSubscribe() {
  KitRuntimeOptions options;
  options.callBudget = 1;
  options.budgetWindowMs = 60000;
  Held h = hold(options);
  std::optional<KitReply> called;
  h.rt->call("world", "", "harvest", JVal(), [&](KitReply r) { called = std::move(r); });
  CHECK(h.gw.until([&] { return called.has_value(); }));
  auto wire = h.gw.transport->all()[0];
  CHECK_EQ(countTag(wire, 0x02), 0);
  std::optional<KitReply> subbed;
  const auto handle = h.rt->subscribe("world", "", "conditions", [](const domains::ExecPush&) {}, [&](KitReply r) { subbed = std::move(r); });
  CHECK(handle != 0);
  CHECK_EQ(countTag(wire, 0x02), 0);
  h.rt->close();
  CHECK(h.gw.until([&] { return subbed.has_value(); }));
  CHECK(subbed->status == ExecStatus::Unavailable);
  CHECK_EQ(subbed->message(), "the kit is closed");
  CHECK_EQ(countTag(wire, 0x02), 0);
}

void testInFlightCallSurvivesRuntime() {
  KitRuntimeOptions options;
  options.callBudget = 2;
  options.budgetWindowMs = 60000;
  Held h = hold(options);
  // The dial is posted, so the socket exists only after a drain, and it is not open yet.
  // Open it by hand: until()'s pump would also answer the call.
  CHECK(h.gw.until([&] { return !h.gw.transport->all().empty(); }));
  auto wire = h.gw.transport->all()[0];
  wire->open();
  h.gw.dispatcher->drain();
  std::optional<KitReply> inflight;
  h.rt->call("world", "", "harvest", JVal(), [&](KitReply r) { inflight = std::move(r); });
  h.gw.dispatcher->drain();
  CHECK_EQ(countTag(wire, 0x01), 1);
  Frame sent{};
  for (const auto& b : wire->frames()) {
    if (!b.empty() && static_cast<std::uint8_t>(b[0]) == 0x01) sent = parse(b);
  }
  h.rt.reset();
  CHECK(!inflight.has_value());
  wire->deliver(reply(sent.rid, 0, Json::parse("true").toMsgpack()));
  h.gw.dispatcher->drain();
  CHECK(inflight.has_value());
  CHECK(inflight->ok());
}

void testCloseRemovesConnectionListeners() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  std::atomic<int> dials{0};
  domains::ExecDial dial = [&](std::function<void(Result<domains::ExecEndpoint>)> found) {
    const int n = ++dials;
    found(domains::ExecEndpoint{"wss://gw.example", "tok" + std::to_string(n), "host-" + std::to_string(n)});
  };
  domains::ExecConnectOptions opts;
  opts.callTimeoutMs = 2000;
  opts.initialReconnectDelayMs = 10;
  auto conn = std::make_shared<domains::ExecConnection>(transport, dispatcher, dial, opts);
  std::optional<Status> opened;
  conn->connect([&](Status s) { opened = s; });
  const auto wait = [&](auto&& done, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      dispatcher->drain();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    dispatcher->drain();
    return done();
  };
  CHECK(wait([&] { return transport->all().size() == 1; }));
  transport->all()[0]->open();
  CHECK(wait([&] { return opened.has_value() && opened->ok(); }));

  KitRuntimeOptions options;
  options.connection = conn;
  auto live = std::make_shared<KitRuntime>("77", nullptr, dispatcher, options);
  auto gone = std::make_shared<KitRuntime>("77", nullptr, dispatcher, options);
  int liveRe = 0, goneRe = 0, liveEnd = 0, goneEnd = 0;
  live->onReconnect([&](std::string) { ++liveRe; });
  gone->onReconnect([&](std::string) { ++goneRe; });
  live->onAccessEnded([&](std::string) { ++liveEnd; });
  gone->onAccessEnded([&](std::string) { ++goneEnd; });
  CHECK(live->connection());
  CHECK(gone->connection());
  gone->close();
  gone.reset();

  transport->all()[0]->drop();
  CHECK(wait([&] { return transport->all().size() == 2; }));
  transport->all()[1]->open();
  CHECK(wait([&] { return liveRe == 1; }));
  CHECK_EQ(goneRe, 0);

  WebSocketEvent ended;
  ended.kind = WebSocketEventKind::Close;
  ended.close.code = domains::kExecCloseAccessEnded;
  ended.close.reason = "signed out";
  transport->all()[1]->emit(ended);
  CHECK(wait([&] { return liveEnd == 1; }));
  CHECK_EQ(goneEnd, 0);
  CHECK_EQ(liveRe, 1);
}

void testSubscriptionCap() {
  Scripted gw;
  gw.answer = [](const Frame&) -> std::pair<std::uint8_t, std::string> { return {0, ""}; };
  KitRuntimeOptions options;
  options.callBudget = 100000;
  auto rt = gw.runtime(options);
  std::vector<std::uint64_t> handles;
  for (std::size_t i = 0; i < kKitSubscriptionsMax; ++i) handles.push_back(rt->subscribe("t", std::to_string(i), "x", [](const domains::ExecPush&) {}));
  CHECK_EQ(rt->subscriptions(), kKitSubscriptionsMax);
  std::optional<KitReply> refused;
  CHECK_EQ(rt->subscribe("t", "one-more", "x", [](const domains::ExecPush&) {}, [&](KitReply r) { refused = r; }), 0u);
  CHECK(gw.until([&] { return refused.has_value(); }));
  CHECK_EQ(refused->code(), "limit");
  // A second handler on a held topic is not another subscription.
  const auto again = rt->subscribe("t", "0", "x", [](const domains::ExecPush&) {});
  CHECK(again != 0);
  CHECK_EQ(rt->subscriptions(), kKitSubscriptionsMax);
  rt->unsubscribe(handles[0]);
  rt->unsubscribe(again);
  CHECK_EQ(rt->subscriptions(), kKitSubscriptionsMax - 1);
}

std::string kitMessage(std::int64_t epoch, std::int64_t seq, const char* kind, const char* body) {
  const Json b = Json::parse(body);
  std::string text = R"({"v":1,"epoch":)" + std::to_string(epoch) + R"(,"seq":)" + std::to_string(seq) + R"(,"kind":")" + kind + "\"";
  b.forEachMember([&](std::string_view k, const Json& v) { text += "," + JVal(std::string(k)).dump() + ":" + v.dump(); });
  text += "}";
  return Json::parse(text).toMsgpack();
}

void testMirrorResyncs() {
  Scripted gw;
  int snapshots = 0;
  gw.answer = [&](const Frame& f) -> std::pair<std::uint8_t, std::string> {
    if (f.name == "snapshot") {
      ++snapshots;
      return {0, kitMessage(snapshots == 1 ? 1 : 2, snapshots == 1 ? 3 : 10, "snapshot",
                            snapshots == 1 ? R"({"containers":[{"id":1,"type_name":"Mob","props":{"hp":5}}]})"
                                           : R"({"containers":[{"id":1,"type_name":"Mob","props":{"hp":2}}]})")};
    }
    return {0, ""};
  };
  auto rt = gw.runtime();
  CollectionMirror area(rt, "npcs", "0,0", "area");
  std::optional<KitReply> started;
  int changes = 0;
  area.subscribe([&] { ++changes; });
  area.start([&](KitReply r) { started = r; });
  CHECK(gw.until([&] { return started.has_value(); }));
  CHECK(started->ok());
  CHECK_EQ(area.member(1)->props["hp"].asInt64(), 5);
  auto conn = gw.transport->all()[0];
  // The next delta applies in order.
  conn->deliver(push("npcs", "0,0", "area", kitMessage(1, 4, "delta", R"({"id":2,"type_name":"Pet","props":{"name":"Mog"}})")));
  CHECK(gw.until([&] { return area.size() == 2; }));
  CHECK_EQ(area.list("Pet").size(), 1u);
  // A gap resyncs.
  conn->deliver(push("npcs", "0,0", "area", kitMessage(1, 9, "delta", R"({"id":2,"removed":true})")));
  CHECK(gw.until([&] { return snapshots == 2 && area.position() == std::make_pair<std::int64_t, std::int64_t>(2, 10); }));
  CHECK_EQ(area.member(1)->props["hp"].asInt64(), 2);
  CHECK(!area.member(2).has_value());
  // An event reaches its listeners; a message from an old epoch's seq is not applied.
  std::string seen;
  area.onEvent([&](const Json& body, const KitHeader&) { seen = body["type"].asString(); });
  conn->deliver(push("npcs", "0,0", "area", kitMessage(2, 11, "event", R"({"type":"roar"})")));
  CHECK(gw.until([&] { return seen == "roar"; }));
  CHECK(changes >= 3);
  area.stop();
}

void testPrivateState() {
  Scripted gw;
  int reads = 0;
  gw.answer = [&](const Frame& f) -> std::pair<std::uint8_t, std::string> {
    if (f.name == "hand") {
      ++reads;
      return {0, Json::parse(reads == 1 ? R"(["7H"])" : R"(["7H","8S"])").toMsgpack()};
    }
    return {0, ""};
  };
  auto rt = gw.runtime();
  PrivateState hand(rt, "deck", "t1", "table", {"hand"}, [rt](KitCallback cb) { rt->call("deck", "t1", "hand", JVal(), std::move(cb)); });
  std::optional<KitReply> started;
  hand.start([&](KitReply r) { started = r; });
  CHECK(gw.until([&] { return started.has_value(); }));
  CHECK_EQ(hand.get().size(), 1u);
  auto conn = gw.transport->all()[0];
  // A change for someone else is not read; one for this player is.
  conn->deliver(push("deck", "t1", "table", kitMessage(1, 1, "changed", R"({"who":[2],"what":"hand"})")));
  conn->deliver(push("deck", "t1", "table", kitMessage(1, 2, "changed", R"({"who":[1],"what":"pile"})")));
  for (int i = 0; i < 50; ++i) gw.until([] { return false; }, 1);
  CHECK_EQ(reads, 1);
  conn->deliver(push("deck", "t1", "table", kitMessage(1, 3, "changed", R"({"who":[1,2],"what":"hand"})")));
  CHECK(gw.until([&] { return hand.get().size() == 2; }));
  CHECK_EQ(reads, 2);
}

// ---- the topology's checks ----

void testManifestProblems() {
  auto problems = [](const char* manifest) { return kitManifestProblems(Json::parse(manifest)); };
  CHECK(problems(R"({"root":"world","types":{"world":{"kind":"hub"},"session":{"kind":"hub","parent":"world","calls":["world"]}}})").empty());
  CHECK(!problems(R"({"root":"world","types":{}})").empty());
  CHECK(!problems(R"({"root":"world","types":{"world":{"kind":"spoke"}}})").empty());
  auto many = problems(R"({"root":"world","types":{"world":{"kind":"hub"},"a b":{"kind":"hub"},"x":{"kind":"hub","parent":"nope","calls":["ghost"],"replicas":2,"scopes":["root.everything"]}}})");
  CHECK_EQ(many.size(), 6u);
}

}  // namespace

int main() {
  testMessages();
  testRefusals();
  testShards();
  testEvents();
  testActorStates();
  testWorld();
  testMatchmaking();
  testSessions();
  testNpcs();
  testLeaderboards();
  testTables();
  testTactics();
  testEncounters();
  testMinigames();
  testInstances();
  testArenas();
  testRace();
  testBall();
  testTerritory();
  testZone();
  testPlayer();
  testMarket();
  testResends();
  testPacing();
  testCloseSettlesQueuedWork();
  testCloseFlushesPacedUnsubscribe();
  testCloseFailsQueuedSubscribe();
  testInFlightCallSurvivesRuntime();
  testCloseRemovesConnectionListeners();
  testSubscriptionCap();
  testMirrorResyncs();
  testPrivateState();
  testManifestProblems();
  std::printf("kit_runtime_test: ok\n");
  return 0;
}
