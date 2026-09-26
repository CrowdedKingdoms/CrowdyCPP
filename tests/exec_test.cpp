#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/json.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::domains;
using graphql::WebSocketEvent;
using graphql::WebSocketEventKind;

namespace {

std::string hex(std::string_view b) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : b) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0xf]);
  }
  return out;
}

std::string unhex(std::string_view h) {
  std::string out;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
  return out;
}

// ---- the golden frames (ck-exec crates/ckx-proto/tests/client-frames.json) ----

void testGoldenFrames() {
  std::ifstream in(std::string(CROWDY_PARITY_FIXTURE_DIR) + "/exec-client-frames.json");
  std::stringstream text;
  text << in.rdbuf();
  graphql::Json fx = graphql::Json::parse(text.str());
  CHECK(fx.ok());
  CHECK(fx["client"].size() > 0);
  fx["client"].forEach([](graphql::Json e) {
    graphql::Json f = e["frame"];
    const std::string kind = f["kind"].asString();
    exec_wire::ClientFrame c;
    if (kind == "ping") {
      c.kind = exec_wire::ClientFrame::Kind::Ping;
      c.rid = static_cast<std::uint32_t>(f["nonce"].asInt64());
    } else {
      c.kind = kind == "call" ? exec_wire::ClientFrame::Kind::Call
               : kind == "subscribe" ? exec_wire::ClientFrame::Kind::Subscribe
                                     : exec_wire::ClientFrame::Kind::Unsubscribe;
      c.rid = static_cast<std::uint32_t>(f["rid"].asInt64());
      c.nodeType = f["nodeType"].asString();
      c.key = f["key"].asString();
      c.method = kind == "call" ? f["method"].asString() : f["topic"].asString();
      c.payload = unhex(f["payloadHex"].asString());
    }
    Result<std::string> bytes = exec_wire::encode(c);
    CHECK(bytes.ok());
    CHECK_EQ(hex(bytes.value()), e["hex"].asString());
  });
  CHECK(fx["server"].size() > 0);
  fx["server"].forEach([](graphql::Json e) {
    graphql::Json f = e["frame"];
    Result<exec_wire::ServerFrame> got = exec_wire::decode(unhex(e["hex"].asString()));
    CHECK(got.ok());
    const std::string kind = f["kind"].asString();
    if (kind == "reply") {
      CHECK(got->kind == exec_wire::ServerFrame::Kind::Reply);
      CHECK_EQ(got->rid, static_cast<std::uint32_t>(f["rid"].asInt64()));
      CHECK_EQ(got->status, static_cast<std::uint8_t>(f["status"].asInt64()));
      CHECK_EQ(hex(got->payload), f["payloadHex"].asString());
    } else if (kind == "push") {
      CHECK(got->kind == exec_wire::ServerFrame::Kind::Push);
      CHECK_EQ(got->nodeType, f["nodeType"].asString());
      CHECK_EQ(got->key, f["key"].asString());
      CHECK_EQ(got->topic, f["topic"].asString());
      CHECK_EQ(hex(got->payload), f["payloadHex"].asString());
    } else {
      CHECK(got->kind == exec_wire::ServerFrame::Kind::Pong);
      CHECK_EQ(got->rid, static_cast<std::uint32_t>(f["nonce"].asInt64()));
    }
  });
  CHECK(!exec_wire::decode(unhex("8101")).ok());
  CHECK(!exec_wire::decode(unhex("99")).ok());
  exec_wire::ClientFrame tooLong{exec_wire::ClientFrame::Kind::Call, 1, std::string(256, 'x'), "", "m", ""};
  CHECK(!exec_wire::encode(tooLong).ok());
}

void testStatusesAndDigests() {
  CHECK_EQ(execStatusName(execStatusFromWire(3)), "Moved");
  CHECK(execStatusFromWire(200) == ExecStatus::Unknown);
  CHECK(execStatusRetryable(ExecStatus::Busy));
  CHECK(!execStatusRetryable(ExecStatus::AppError));

  const std::string refusal =
      "rate limited: a player may make 120 calls per 10000 ms to an app on one host; retry in 250 ms";
  ExecReply limited{ExecStatus::Busy, refusal};
  CHECK(limited.retryable());
  CHECK(limited.rateLimited());
  CHECK(limited.retryAfterMs() == std::optional<long>(250));
  ExecReply full{ExecStatus::Busy, "mailbox full"};
  CHECK(full.retryable());
  CHECK(!full.rateLimited());
  CHECK(!full.retryAfterMs().has_value());
  CHECK((ExecReply{ExecStatus::RateLimited, "slow down"}.rateLimited()));
  CHECK(!(ExecReply{ExecStatus::Busy, "rate limited: retry in soon"}.retryAfterMs().has_value()));
  CHECK(!(ExecReply{ExecStatus::Denied, refusal}.rateLimited()));
  CHECK_EQ(execSha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_EQ(execSha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_EQ(execSha256Hex(unhex("0061736d01000000")), "93a44bbb96c751218e4c00d479e4c14358122a389acca16205b1e4d0dc5f9476");
  CHECK_EQ(execSha256Hex(std::string(1000, 'a')), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

// ---- the connection, on a fake transport ----

struct FakeConn : graphql::IWebSocketConnection {
  std::string url;
  std::string subprotocol;
  std::mutex mu;
  graphql::WebSocketEventCallback cb;
  std::vector<std::string> sent;
  bool closed = false;

  void start(graphql::WebSocketEventCallback c) override {
    std::lock_guard lock(mu);
    cb = std::move(c);
  }
  Status send(graphql::WebSocketFrame f) override {
    std::lock_guard lock(mu);
    sent.push_back(std::move(f.payload));
    return Errc::Ok;
  }
  void close(std::uint16_t, std::string_view) override {
    std::lock_guard lock(mu);
    closed = true;
  }
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
    c->subprotocol = r.subprotocol;
    std::lock_guard lock(mu);
    conns.push_back(c);
    return c;
  }
  std::shared_ptr<FakeConn> at(std::size_t i) {
    std::lock_guard lock(mu);
    return i < conns.size() ? conns[i] : nullptr;
  }
  std::size_t count() {
    std::lock_guard lock(mu);
    return conns.size();
  }
};

/// A sent client frame, parsed back.
struct Sent {
  std::uint8_t tag = 0;
  std::uint32_t rid = 0;
  std::string nodeType, key, name, payload;
};

Sent parse(const std::string& b) {
  Sent s;
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

template <typename Fn>
bool until(graphql::Dispatcher& d, Fn&& done, int ms = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    d.drain();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  d.drain();
  return done();
}

/// The newest frame on `c` whose tag is `tag`.
std::optional<Sent> last(const std::shared_ptr<FakeConn>& c, std::uint8_t tag) {
  std::optional<Sent> found;
  for (const auto& b : c->frames()) {
    Sent s = parse(b);
    if (s.tag == tag) found = s;
  }
  return found;
}

void testConnection() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  std::atomic<int> dials{0};
  ExecDial dial = [&dials](std::function<void(Result<ExecEndpoint>)> found) {
    const int n = ++dials;
    found(ExecEndpoint{"wss://gw" + std::to_string(n) + ".example/", "tok" + std::to_string(n), "host-" + std::to_string(n)});
  };
  ExecConnectOptions opts;
  opts.callTimeoutMs = 2000;
  opts.initialReconnectDelayMs = 10;
  auto conn = std::make_shared<ExecConnection>(transport, dispatcher, dial, opts);
  std::vector<std::string> reconnected;
  conn->onReconnect([&](std::string h) { reconnected.push_back(std::move(h)); });

  // A call made before the socket opens waits for it.
  std::optional<ExecReply> r1;
  conn->call("arena", "m1", "hit", graphql::JVal::object({{"weapon", graphql::JVal(2)}}), [&](ExecReply r) { r1 = r; });
  CHECK(until(*dispatcher, [&] { return transport->count() == 1; }));
  auto c1 = transport->at(0);
  CHECK_EQ(c1->url, "wss://gw1.example/v1/connect?token=tok1");
  CHECK_EQ(c1->subprotocol, "");
  CHECK(c1->frames().empty());
  c1->open();
  auto call = last(c1, 0x01);
  CHECK(call.has_value());
  CHECK_EQ(call->name, "hit");
  CHECK_EQ(graphql::Json::fromMsgpack(call->payload)["weapon"].asInt64(), 2);
  c1->deliver(reply(call->rid, 0, graphql::Json::parse(R"({"hp":9})").toMsgpack()));
  CHECK(until(*dispatcher, [&] { return r1.has_value(); }));
  CHECK(r1->ok());
  CHECK_EQ(r1->value()["hp"].asInt64(), 9);
  CHECK_EQ(conn->host(), "host-1");

  // A refusal carries the platform status and the handler's message.
  std::optional<ExecReply> r2;
  conn->callRaw("arena", "m1", "fail", "", [&](ExecReply r) { r2 = r; });
  call = last(c1, 0x01);
  c1->deliver(reply(call->rid, 1, "asked to fail"));
  CHECK(until(*dispatcher, [&] { return r2.has_value(); }));
  CHECK(r2->status == ExecStatus::AppError);
  CHECK_EQ(r2->message(), "asked to fail");

  // A call over the player's call limit is Busy with its retry hint, and is not sent again.
  std::optional<ExecReply> limited;
  const auto sentBefore = c1->frames().size();
  conn->callRaw("arena", "m1", "spam", "", [&](ExecReply r) { limited = r; });
  call = last(c1, 0x01);
  c1->deliver(reply(call->rid, 2,
                    "rate limited: a player may make 120 calls per 10000 ms to an app on one host; retry in 250 ms"));
  CHECK(until(*dispatcher, [&] { return limited.has_value(); }));
  CHECK(limited->status == ExecStatus::Busy);
  CHECK(limited->rateLimited());
  CHECK(limited->retryAfterMs() == std::optional<long>(250));
  dispatcher->drain();
  CHECK_EQ(c1->frames().size(), sentBefore + 1);

  // Pushes reach the handler decoded.
  std::vector<std::int64_t> hps;
  std::optional<ExecReply> subbed;
  const auto handle = conn->subscribe("arena", "m1", "hp", [&](const ExecPush& p) { hps.push_back(p.value()["hp"].asInt64()); },
                                      [&](ExecReply r) { subbed = r; });
  auto sub = last(c1, 0x02);
  CHECK(sub.has_value());
  CHECK_EQ(sub->name, "hp");
  c1->deliver(reply(sub->rid, 0, ""));
  c1->deliver(push("arena", "m1", "hp", graphql::Json::parse(R"({"hp":7})").toMsgpack()));
  CHECK(until(*dispatcher, [&] { return subbed.has_value() && hps.size() == 1; }));
  CHECK(subbed->ok());
  CHECK_EQ(hps[0], 7);

  // The host goes away with a call in flight: a new host, the call sent again there,
  // the subscription renewed.
  std::optional<ExecReply> r3;
  conn->call("arena", "m1", "state", graphql::JVal(), [&](ExecReply r) { r3 = r; });
  CHECK(last(c1, 0x01)->name == "state");
  c1->drop();
  CHECK(until(*dispatcher, [&] { return transport->count() == 2; }));
  auto c2 = transport->at(1);
  CHECK_EQ(c2->url, "wss://gw2.example/v1/connect?token=tok2");
  c2->open();
  CHECK(until(*dispatcher, [&] { return reconnected.size() == 1; }));
  CHECK_EQ(reconnected[0], "host-2");
  auto renewed = last(c2, 0x02);
  CHECK(renewed.has_value());
  CHECK_EQ(renewed->name, "hp");
  auto again = last(c2, 0x01);
  CHECK(again.has_value());
  CHECK_EQ(again->name, "state");
  c2->deliver(reply(again->rid, 0, graphql::Json::parse("5").toMsgpack()));
  CHECK(until(*dispatcher, [&] { return r3.has_value(); }));
  CHECK(r3->ok());
  CHECK_EQ(r3->value().asInt64(), 5);

  // Moved: once, on the host the Game API picks now.
  std::optional<ExecReply> r4;
  conn->call("arena", "m1", "state", graphql::JVal(), [&](ExecReply r) { r4 = r; });
  auto moved = last(c2, 0x01);
  c2->deliver(reply(moved->rid, 3, "moved"));
  CHECK(until(*dispatcher, [&] { return transport->count() == 3; }));
  auto c3 = transport->at(2);
  c3->open();
  CHECK(until(*dispatcher, [&] { return last(c3, 0x01).has_value(); }));
  auto retried = last(c3, 0x01);
  c3->deliver(reply(retried->rid, 0, graphql::Json::parse("6").toMsgpack()));
  CHECK(until(*dispatcher, [&] { return r4.has_value(); }));
  CHECK(r4->ok());
  CHECK_EQ(r4->value().asInt64(), 6);
  CHECK_EQ(conn->host(), "host-3");

  // The last handler gone, the gateway is told.
  conn->unsubscribe(handle);
  CHECK(until(*dispatcher, [&] { return last(c3, 0x03).has_value(); }));

  // Closed: calls in flight fail, nothing reconnects.
  std::optional<ExecReply> r5;
  conn->call("arena", "m1", "state", graphql::JVal(), [&](ExecReply r) { r5 = r; });
  conn->close();
  CHECK(until(*dispatcher, [&] { return r5.has_value(); }));
  CHECK(r5->status == ExecStatus::Unavailable);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(transport->count(), 3u);
}

void testCallTimesOut() {
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  ExecConnectOptions opts;
  opts.callTimeoutMs = 60;
  auto conn = ExecConnection::open(transport, dispatcher, ExecEndpoint{"ws://gw", "t", "h"}, opts);
  CHECK(until(*dispatcher, [&] { return transport->count() == 1; }));
  transport->at(0)->open();
  std::optional<ExecReply> r;
  conn->callRaw("probe", "", "spin", "", [&](ExecReply x) { r = x; });
  CHECK(until(*dispatcher, [&] { return r.has_value(); }));
  CHECK(r->status == ExecStatus::DeadlineExceeded);
  conn->close();
}

}  // namespace

// ---- operations, on a fake GraphQL endpoint ----

// Answers each operation by name and keeps every request body.
class RecordingHttp final : public graphql::IHttpTransport {
 public:
  std::vector<graphql::Json> requests;

  graphql::HttpResponse send(const graphql::HttpRequest& r) override {
    auto body = graphql::Json::parse(r.body);
    requests.push_back(body);
    const auto op = body["operationName"].asString();
    const std::string status = R"({"activeVersion":2,"disabled":false,"disabledTypes":["bare"],"budgetPaused":false})";
    if (op == "ExecLogs")
      return {200, R"({"data":{"execLogs":[{"id":"9","nodeType":"arena","key":"m1","level":2,"host":"h","at":"2026-09-25T00:00:00.000Z","text":"hi","flow":"0123456789abcdef0123456789abcdef"}]}})"};
    if (op == "ExecVersions")
      return {200, R"({"data":{"execVersions":[{"version":2,"createdBy":"user:1","createdAt":"t","types":1,"active":true,"manifestJson":"{\"root\":\"lobby\",\"types\":{\"lobby\":{\"kind\":\"hub\",\"seed_bytes\":4}}}"},{"version":1,"createdBy":"user:1","createdAt":"t","types":1,"active":false,"manifestJson":null}]}})"};
    if (op == "ExecEndpointStats")
      return {200, R"({"data":{"execEndpointStats":[{"nodeType":"arena","method":"hit","calls":40,"appErrors":1,"busy":3,"denied":0,"deadlineExceeded":0,"otherErrors":0,"timedCalls":37,"latencyMsAvg":1.5,"latencyMsMax":9,"firstMinute":"t0","lastMinute":"t1"}]}})"};
    if (op == "ExecSetEnabled") return {200, R"({"data":{"execSetEnabled":)" + status + "}}"};
    if (op == "ExecActivateVersion") return {200, R"({"data":{"execActivateVersion":)" + status + "}}"};
    if (op == "ExecConnectAsDeveloper")
      return {200, R"({"data":{"execConnectAsDeveloper":{"gatewayUrl":"wss://gw","token":"dev-1","host":"h2","expiresAt":"2026-09-25T00:01:00.000Z"}}})"};
    if (op == "ExecStarters")
      return {200, R"({"data":{"execStarters":{"manifestJson":"{\"root\":\"world\"}","starters":[{"crate":"world-tick","nodeType":"world","description":"d","files":[{"path":"Cargo.toml","content":"c"}]}]}}})"};
    const std::string build = R"({"buildId":"b1","log":null,"createdAt":"t","startedAt":null,"finishedAt":null,"artifacts":[)";
    if (op == "ExecBuild") return {200, R"({"data":{"execBuild":)" + build + R"(],"status":"queued"}}})"};
    if (op == "ExecBuildStatus") {
      const bool done = ++statusCalls >= 2;
      return {200, R"({"data":{"execBuildStatus":)" + build +
                       (done ? R"({"crate":"world-tick","digest":"ab","sizeBytes":9}],"status":"succeeded"}}})"
                             : R"(],"status":"building"}}})")};
    }
    if (op == "ExecDeploy") return {200, R"({"data":{"execDeploy":{"version":5}}})"};
    if (op == "ExecModBuild") return {200, R"({"data":{"execModBuild":)" + build + R"(],"status":"queued"}}})"};
    if (op == "ExecModBuildStatus") {
      const bool done = ++modStatusCalls >= 2;
      return {200, R"({"data":{"execModBuildStatus":)" + build +
                       (done ? R"({"crate":"grid-mod","digest":"cd","sizeBytes":7}],"status":"succeeded"}}})"
                             : R"(],"status":"building"}}})")};
    }
    const std::string mod =
        R"({"modId":"900","gridId":"5","name":"turret","ownerId":"42","version":1,"digest":"ab","enabled":false,"listingId":null,"blocked":null,"running":false,"updatedAt":"t"})";
    if (op == "ExecModDeploy") return {200, R"({"data":{"execModDeploy":)" + mod + "}}"};
    if (op == "ExecModSetEnabled") return {200, R"({"data":{"execModSetEnabled":)" + mod + "}}"};
    if (op == "ExecModSetSwitch")
      return {200, R"({"data":{"execModSetSwitch":[{"scope":"GRID","target":"5","reason":"r","createdBy":"user:1","createdAt":"t"}]}})"};
    return {200, R"({"data":{}})"};
  }

  int statusCalls = 0;
  int modStatusCalls = 0;

#ifdef CROWDY_NO_EXCEPTIONS
  graphql::HttpOutcome sendOutcome(const graphql::HttpRequest& r) noexcept override {
    return {Errc::Ok, send(r), {}};
  }
#endif
};

void testOperations() {
  auto http = std::make_shared<RecordingHttp>();
  auto gql = std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
                                                      std::make_shared<graphql::AuthState>());
  ExecAPI exec(gql, std::make_shared<FakeTransport>());

  ExecLogsQuery q;
  q.nodeType = "arena";
  q.maxLevel = 1;
  q.limit = 10;
  auto lines = exec.logs("77", q);
  CHECK_EQ(lines.size(), 1u);
  CHECK_EQ(lines.at(0)["text"].asString(), std::string("hi"));
  auto vars = http->requests.back()["variables"];
  CHECK_EQ(vars["appId"].asString(), std::string("77"));
  CHECK_EQ(vars["nodeType"].asString(), std::string("arena"));
  CHECK_EQ(vars["maxLevel"].asInt64(), 1);
  CHECK_EQ(vars["limit"].asInt64(), 10);
  CHECK(vars["key"].isNull());
  CHECK(vars["before"].isNull());
  CHECK(vars["flow"].isNull());
  CHECK(lines.at(0)["flow"].isString());

  q.flow = "0123456789abcdef0123456789abcdef";
  exec.logs("77", q);
  CHECK_EQ(http->requests.back()["variables"]["flow"].asString(), q.flow);

  auto versions = exec.versions("77");
  CHECK_EQ(versions.size(), 2u);
  auto manifest = graphql::Json::parse(versions.at(0)["manifestJson"].asString());
  CHECK_EQ(manifest["root"].asString(), std::string("lobby"));
  CHECK_EQ(manifest["types"]["lobby"]["seed_bytes"].asInt64(), 4);
  CHECK(versions.at(1)["manifestJson"].isNull());

  auto stats = exec.endpointStats("77", "arena", 30);
  CHECK_EQ(stats.at(0)["busy"].asInt64(), 3);
  CHECK_EQ(stats.at(0)["latencyMsAvg"].asDouble(), 1.5);
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecEndpointStats"));
  CHECK_EQ(vars["nodeType"].asString(), std::string("arena"));
  CHECK_EQ(vars["sinceMinutes"].asInt64(), 30);
  exec.endpointStats("77");
  vars = http->requests.back()["variables"];
  CHECK(vars["nodeType"].isNull());
  CHECK(vars["sinceMinutes"].isNull());

  auto s = exec.setEnabled("77", false, "bare");
  CHECK_EQ(s["disabledTypes"].at(0).asString(), std::string("bare"));
  vars = http->requests.back()["variables"];
  CHECK(vars["enabled"].isBool() && !vars["enabled"].asBool());
  CHECK_EQ(vars["nodeType"].asString(), std::string("bare"));

  auto a = exec.activateVersion("77", 2);
  CHECK_EQ(a["activeVersion"].asInt64(), 2);
  CHECK_EQ(http->requests.back()["variables"]["version"].asInt64(), 2);

  auto dev = exec.developerEndpoint("77", "bare", "k");
  CHECK(dev.ok());
  CHECK_EQ(dev.value().token, std::string("dev-1"));
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecConnectAsDeveloper"));
  CHECK_EQ(http->requests.back()["variables"]["key"].asString(), std::string("k"));
}

void testBuilds() {
  auto http = std::make_shared<RecordingHttp>();
  auto gql = std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
                                                      std::make_shared<graphql::AuthState>());
  ExecAPI exec(gql, std::make_shared<FakeTransport>());

  auto pack = exec.starters("77");
  CHECK_EQ(pack["starters"].at(0)["crate"].asString(), std::string("world-tick"));
  CHECK_EQ(pack["manifestJson"].asString(), std::string("{\"root\":\"world\"}"));

  auto queued = exec.build("77", {ExecCrate{"world-tick", {{"Cargo.toml", "c"}, {"src/lib.rs", "l"}}}});
  CHECK_EQ(queued["status"].asString(), std::string("queued"));
  auto input = http->requests.back()["variables"]["input"];
  CHECK_EQ(input["appId"].asString(), std::string("77"));
  CHECK_EQ(input["crates"].at(0)["name"].asString(), std::string("world-tick"));
  CHECK_EQ(input["crates"].at(0)["files"].at(1)["path"].asString(), std::string("src/lib.rs"));

  auto done = exec.waitForBuild("77", "b1", 1, 10000);
  CHECK_EQ(done["status"].asString(), std::string("succeeded"));
  CHECK_EQ(done["artifacts"].at(0)["crate"].asString(), std::string("world-tick"));
  CHECK_EQ(http->statusCalls, 2);
  CHECK_EQ(http->requests.back()["variables"]["buildId"].asString(), std::string("b1"));

  // A deploy of the build: the crate-named type uploads nothing, the one with bytes does.
  ExecNodeType world;
  world.name = "world";
  world.kind = "hub";
  world.crate = "world-tick";
  world.client = true;
  ExecNodeType extra;
  extra.name = "extra";
  extra.kind = "spoke";
  extra.parent = "world";
  extra.wasm = std::string("\0asm\1\0\0\0", 8);
  auto v = exec.deploy("77", "world", {world, extra}, "b1");
  CHECK_EQ(v["version"].asInt64(), 5);
  input = http->requests.back()["variables"]["input"];
  CHECK_EQ(input["buildId"].asString(), std::string("b1"));
  auto manifest = graphql::Json::parse(input["manifestJson"].asString());
  CHECK_EQ(manifest["types"]["world"]["crate"].asString(), std::string("world-tick"));
  CHECK(manifest["types"]["world"]["digest"].isNull());
  CHECK_EQ(manifest["types"]["extra"]["digest"].asString().size(), 64u);
  CHECK_EQ(input["artifacts"].size(), 1u);
}

void testMods() {
  auto http = std::make_shared<RecordingHttp>();
  auto gql = std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
                                                      std::make_shared<graphql::AuthState>());
  ExecAPI exec(gql, std::make_shared<FakeTransport>());

  auto queued = exec.modBuild("77", ExecCrate{"grid-mod", {{"Cargo.toml", "c"}, {"src/lib.rs", "l"}}});
  CHECK_EQ(queued["status"].asString(), std::string("queued"));
  auto vars = http->requests.back()["variables"];
  CHECK_EQ(vars["appId"].asString(), std::string("77"));
  CHECK_EQ(vars["crate"]["name"].asString(), std::string("grid-mod"));
  CHECK_EQ(vars["crate"]["files"].at(1)["path"].asString(), std::string("src/lib.rs"));
  auto done = exec.waitForModBuild("77", "b1", 1, 10000);
  CHECK_EQ(done["status"].asString(), std::string("succeeded"));
  CHECK_EQ(http->modStatusCalls, 2);

  auto m = exec.modDeploy("77", "5", "turret", "b1");
  CHECK_EQ(m["ownerId"].asString(), std::string("42"));
  vars = http->requests.back()["variables"];
  CHECK_EQ(vars["gridId"].asString(), std::string("5"));
  CHECK_EQ(vars["name"].asString(), std::string("turret"));
  CHECK_EQ(vars["buildId"].asString(), std::string("b1"));
  exec.modSetEnabled("77", "5", "turret", true);
  CHECK(http->requests.back()["variables"]["enabled"].asBool());

  auto off = exec.modSetSwitch("77", gen::ExecModScope::GRID, true, "5", "r");
  CHECK_EQ(off.at(0)["scope"].asString(), std::string("GRID"));
  vars = http->requests.back()["variables"];
  CHECK_EQ(vars["scope"].asString(), std::string("GRID"));
  CHECK_EQ(vars["target"].asString(), std::string("5"));
  CHECK(vars["off"].asBool());
  exec.modSetSwitch("77", gen::ExecModScope::ALL, false);
  CHECK(http->requests.back()["variables"]["target"].isNull());

  CHECK_EQ(execModType("turret"), std::string("mod:turret"));
}

int main() {
  testGoldenFrames();
  testStatusesAndDigests();
  testConnection();
  testCallTimesOut();
  testOperations();
  testBuilds();
  testMods();
  std::puts("exec_test OK");
  return 0;
}
