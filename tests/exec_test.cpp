#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/core/base64.hpp"
#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/http.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/json.hpp"
#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/studio/runtime.hpp"
#endif
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
    // The second token carries characters a query string must escape.
    found(ExecEndpoint{"wss://gw" + std::to_string(n) + ".example/", "tok" + std::to_string(n) + (n == 2 ? "+/=&" : ""),
                       "host-" + std::to_string(n)});
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
  CHECK_EQ(c2->url, "wss://gw2.example/v1/connect?token=tok2%2B%2F%3D%26");
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
      return {200, R"({"data":{"execConnectAsDeveloper":{"gatewayUrl":")" + gatewayUrl +
                       R"(","token":"dev-1","host":"h2","expiresAt":"2026-09-25T00:01:00.000Z"}}})"};
    if (op == "ExecStarters")
      return {200, R"({"data":{"execStarters":{"manifestJson":"{\"root\":\"world\"}","starters":[{"crate":"world-tick","nodeType":"world","description":"d","files":[{"path":"Cargo.toml","content":"c"}]}]}}})"};
    const std::string build = R"({"buildId":"b1","log":null,"sdkVersion":null,"createdAt":"t","startedAt":null,"finishedAt":null,"artifacts":[)";
    const std::string started = R"({"buildId":"b1","log":null,"sdkVersion":"0.9.0","createdAt":"t","startedAt":"t","finishedAt":null,"artifacts":[)";
    if (op == "ExecBuild") return {200, R"({"data":{"execBuild":)" + build + R"(],"status":"queued"}}})"};
    if (op == "ExecBuildStatus") {
      const bool done = ++statusCalls >= 2;
      return {200, R"({"data":{"execBuildStatus":)" + started +
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
    if (op == "ExecConnect")
      return {200, R"({"data":{"execConnect":{"gatewayUrl":")" + gatewayUrl +
                       R"(","token":"t1","host":"h1","expiresAt":"2026-09-26T00:01:00.000Z"}}})"};
    if (op == "ExecModLogs")
      return {200, R"({"data":{"execModLogs":[{"id":"2","nodeType":"mod:3d-server","key":"5","level":0,"host":"h","at":"2026-09-26T00:00:02.000Z","text":"boom"},{"id":"1","nodeType":"mod:3d-server","key":"5","level":2,"host":"h","at":"2026-09-26T00:00:01.000Z","text":"visited"}]}})"};
    if (op == "ExecMyMods") return {200, R"({"data":{"execMyMods":[]}})"};
    if (op == "ExecModStarter")
      return {200, R"({"data":{"execModStarter":{"crate":"grid-mod","nodeType":"mod","description":"d","files":[{"path":"Cargo.toml","content":"c"},{"path":"src/lib.rs","content":"l"}]}}})"};
    if (op == "ExecModSetSwitch")
      return {200, R"({"data":{"execModSetSwitch":[{"scope":"GRID","target":"5","reason":"r","createdBy":"user:1","createdAt":"t"}]}})"};
    if (op == "ExecModClientBuild")
      return {200, R"({"data":{"execModClientBuild":{"buildId":"cb1","status":"queued","kind":"client","log":null,"createdAt":"t","startedAt":null,"finishedAt":null,"artifacts":[]}}})"};
    if (op == "ExecModClientDeploy")
      return {200, R"({"data":{"execModClientDeploy":{"modId":"900","gridId":"5","name":"turret","ownerId":"42","clientVersion":2,"digest":")" + clientDigest + R"(","sizeBytes":8,"capabilitySummaryJson":"{\"hostFunctions\":[\"hud_set\"]}","capabilityHash":"h1","tickIntervalMs":250,"updatedAt":"t"}}})"};
    if (op == "ExecModClientDelete") return {200, R"({"data":{"execModClientDelete":true}})"};
    if (op == "ExecGridClientMods")
      return {200, R"({"data":{"execGridClientMods":[{"modId":"900","name":"turret","gridId":"5","authorId":"42","listingId":null,"clientVersion":2,"digest":"ab","capabilitySummaryJson":"{\"version\":1,\"target\":\"client\",\"hostFunctions\":[\"hud_set\"]}","capabilityHash":"h1","tickIntervalMs":250,"callerConsented":false,"authorCapabilitySummaryJson":"{\"version\":1,\"target\":\"client\",\"hostFunctions\":[\"hud_set\",\"overlay_draw\"]}","authorCapabilityHash":"a1","callerTrustsAuthor":false,"updatedAt":"t"}]}})"};
    if (op == "ExecConsentClientMod") return {200, R"({"data":{"execConsentClientMod":true}})"};
    if (op == "ExecTrustAuthor") return {200, R"({"data":{"execTrustAuthor":true}})"};
    if (op == "ExecRevokeClientModConsent") return {200, R"({"data":{"execRevokeClientModConsent":true}})"};
    if (op == "ExecRevokeAuthorTrust") return {200, R"({"data":{"execRevokeAuthorTrust":false}})"};
    if (op == "ExecModClientArtifact") {
      if (artifactNotFound)
        return {200, R"({"errors":[{"message":"no such CLIENT half","extensions":{"code":"NOT_FOUND"}}],"data":null})"};
      return {200, R"({"data":{"execModClientArtifact":)" + clientArtifact + "}}"};
    }
    return {200, R"({"data":{}})"};
  }

  int statusCalls = 0;
  int modStatusCalls = 0;
  /// The gateway `execConnect` names: on the test Game API's own host by default.
  std::string gatewayUrl = "wss://test";
  /// What `execModClientArtifact` answers, as JSON.
  std::string clientArtifact = "null";
  /// The digest `execModClientDeploy` reports for the CLIENT half it attached.
  std::string clientDigest = "ab";
  bool artifactNotFound = false;

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
  // The SDK the platform compiled against, whatever the crate names (ck-api v2.40.2).
  CHECK_EQ(done["sdkVersion"].asString(), std::string("0.9.0"));
  CHECK(http->requests.back()["query"].asString().find("sdkVersion") != std::string::npos);

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

// ---- CLIENT halves ----

std::shared_ptr<graphql::GraphQLClient> clientOver(std::shared_ptr<RecordingHttp> http) {
  return std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://test/graphql", 1000},
                                                  std::move(http), std::make_shared<graphql::AuthState>());
}

void testClientHalves() {
  auto http = std::make_shared<RecordingHttp>();
  ExecAPI exec(clientOver(http), std::make_shared<FakeTransport>());
  constexpr auto npos = std::string::npos;

  auto queued = exec.modClientBuild("77", ExecCrate{"hud", {{"Cargo.toml", "c"}, {"src/lib.rs", "l"}}});
  CHECK_EQ(queued["kind"].asString(), std::string("client"));
  auto body = http->requests.back();
  CHECK_EQ(body["operationName"].asString(), std::string("ExecModClientBuild"));
  auto vars = body["variables"];
  CHECK_EQ(vars["appId"].asString(), std::string("77"));
  CHECK_EQ(vars["crate"]["name"].asString(), std::string("hud"));
  CHECK_EQ(vars["crate"]["files"].at(1)["path"].asString(), std::string("src/lib.rs"));
  // Every build selects what a CLIENT build adds: its kind and each module's capability fields.
  for (const std::string_view field : {"kind", "capabilitySummaryJson", "capabilityHash", "tickIntervalMs"}) {
    CHECK(body["query"].asString().find(field) != npos);
  }
  exec.modBuild("77", ExecCrate{"grid-mod", {{"Cargo.toml", "c"}}});
  CHECK(http->requests.back()["query"].asString().find("capabilityHash") != npos);

  auto attached = exec.modClientDeploy("77", "5", "turret", "cb1");
  CHECK_EQ(attached["clientVersion"].asInt64(), 2);
  CHECK_EQ(attached["capabilityHash"].asString(), std::string("h1"));
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecModClientDeploy"));
  CHECK_EQ(vars["gridId"].asString(), std::string("5"));
  CHECK_EQ(vars["name"].asString(), std::string("turret"));
  CHECK_EQ(vars["buildId"].asString(), std::string("cb1"));

  CHECK(exec.modClientDelete("77", "5", "turret").asBool());
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecModClientDelete"));
  CHECK_EQ(vars["name"].asString(), std::string("turret"));
  CHECK(vars["buildId"].isNull());

  auto served = exec.gridClientMods("77", "5");
  CHECK_EQ(served.size(), 1u);
  CHECK_EQ(http->requests.back()["variables"]["gridId"].asString(), std::string("5"));
  CHECK(!served.at(0)["callerConsented"].asBool());
  CHECK(!served.at(0)["callerTrustsAuthor"].asBool());
  auto own = parseExecClientCapabilitySummary(served.at(0)["capabilitySummaryJson"].asStringView());
  auto author = parseExecClientCapabilitySummary(served.at(0)["authorCapabilitySummaryJson"].asStringView());
  CHECK(own.has_value() && author.has_value());
  CHECK_EQ(own->hostFunctions, std::vector<std::string>{"hud_set"});
  CHECK_EQ(author->hostFunctions.size(), 2u);

  CHECK(exec.consentClientMod("77", "900", "h1").asBool());
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecConsentClientMod"));
  CHECK_EQ(vars["modId"].asString(), std::string("900"));
  CHECK_EQ(vars["capabilityHash"].asString(), std::string("h1"));

  CHECK(exec.trustAuthor("77", "5", "42", "a1").asBool());
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecTrustAuthor"));
  CHECK_EQ(vars["gridId"].asString(), std::string("5"));
  CHECK_EQ(vars["authorId"].asString(), std::string("42"));
  CHECK_EQ(vars["capabilityHash"].asString(), std::string("a1"));

  // Taking them back (0.52.0): the consent to one CLIENT half, and the trust in its author.
  CHECK(exec.revokeClientModConsent("77", "900").asBool());
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecRevokeClientModConsent"));
  CHECK_EQ(vars["appId"].asString(), std::string("77"));
  CHECK_EQ(vars["modId"].asString(), std::string("900"));
  CHECK(vars["capabilityHash"].isNull());
  CHECK(!exec.revokeAuthorTrust("77", "5", "42").asBool());
  vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecRevokeAuthorTrust"));
  CHECK_EQ(vars["gridId"].asString(), std::string("5"));
  CHECK_EQ(vars["authorId"].asString(), std::string("42"));
  CHECK(vars["capabilityHash"].isNull());

  // A listing carries the CLIENT half it was published with.
  exec.modListings("77");
  for (const std::string_view field :
       {"clientDigest", "clientCapabilitySummaryJson", "clientCapabilityHash", "clientTickIntervalMs"}) {
    CHECK(http->requests.back()["query"].asString().find(field) != npos);
  }

  // The async twins send the same operations and hand back the root field.
  std::vector<std::string> sent;
  std::vector<graphql::Json> answers;
  auto record = [&](graphql::GraphQLOutcome out) {
    CHECK(out.ok());
    sent.push_back(http->requests.back()["operationName"].asString());
    answers.push_back(out.data);
  };
  http->clientArtifact = R"({"modId":"900"})";
  exec.modClientBuildAsync("77", ExecCrate{"hud", {{"Cargo.toml", "c"}}}, record);
  exec.modClientDeployAsync("77", "5", "turret", "cb1", record);
  exec.modClientDeleteAsync("77", "5", "turret", record);
  exec.gridClientModsAsync("77", "5", record);
  exec.consentClientModAsync("77", "900", "h1", record);
  exec.trustAuthorAsync("77", "5", "42", "a1", record);
  exec.modClientArtifactAsync("77", "900", record);
  exec.revokeClientModConsentAsync("77", "900", record);
  exec.revokeAuthorTrustAsync("77", "5", "42", record);
  const std::vector<std::string> expected = {"ExecModClientBuild",         "ExecModClientDeploy", "ExecModClientDelete",
                                             "ExecGridClientMods",         "ExecConsentClientMod", "ExecTrustAuthor",
                                             "ExecModClientArtifact",      "ExecRevokeClientModConsent",
                                             "ExecRevokeAuthorTrust"};
  CHECK_EQ(sent, expected);
  CHECK_EQ(answers.at(0)["kind"].asString(), std::string("client"));
  CHECK(answers.at(2).asBool());
  CHECK_EQ(answers.at(3).size(), 1u);
  CHECK_EQ(answers.at(6)["modId"].asString(), std::string("900"));
  CHECK(answers.at(7).asBool());
  CHECK(!answers.at(8).asBool());
}

void testCapabilitySummary() {
  CHECK_EQ(kExecClientAbiVersion, 0);
  auto full = parseExecClientCapabilitySummary(
      R"({"version":1,"target":"client","imports":["ck.log","ck.host_call"],"hostFunctions":["hud_set"],)"
      R"("capabilityGroups":["present"],"presentationHooks":["hud_set"],"exportedFunctions":["ck_init","ck_tick"],"extra":{}})");
  CHECK(full.has_value());
  CHECK_EQ(full->version, 1);
  CHECK_EQ(full->target, std::string("client"));
  CHECK_EQ(full->imports, (std::vector<std::string>{"ck.log", "ck.host_call"}));
  CHECK_EQ(full->capabilityGroups, std::vector<std::string>{"present"});
  CHECK_EQ(full->presentationHooks, std::vector<std::string>{"hud_set"});
  CHECK_EQ(full->exportedFunctions, (std::vector<std::string>{"ck_init", "ck_tick"}));
  // A module that reaches no host call still has a bound: the empty one.
  auto none = parseExecClientCapabilitySummary(R"({"hostFunctions":[],"imports":"ck.log"})");
  CHECK(none.has_value() && none->hostFunctions.empty() && none->imports.empty());
  // Anything without a list of host call names bounds nothing.
  for (const std::string_view bad : {"", "not json", "[\"hud_set\"]", "{}", R"({"hostFunctions":"hud_set"})",
                                     R"({"hostFunctions":["hud_set",7]})", "null"}) {
    CHECK(!parseExecClientCapabilitySummary(bad).has_value());
  }
}

std::string upperHex(std::string hex) {
  for (char& c : hex) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  return hex;
}

const char* const kSummary =
    R"({"version":1,"target":"client","imports":["ck.host_call"],"hostFunctions":["hud_set"],)"
    R"("capabilityGroups":["present"],"presentationHooks":["hud_set"],"exportedFunctions":["ck_init","ck_tick"]})";

/// An `execModClientArtifact` answer serving `wasm` under `digest`.
std::string servedArtifact(std::string_view wasm, const std::string& digest, const std::string& summary = kSummary,
                           std::int64_t abi = 0, const std::string& fuel = "100000000") {
  graphql::JVal a;
  a["modId"] = graphql::JVal(std::string("900"));
  a["name"] = graphql::JVal(std::string("turret"));
  a["gridId"] = graphql::JVal(std::string("5"));
  a["clientVersion"] = graphql::JVal(std::int64_t{2});
  a["digest"] = graphql::JVal(digest);
  a["wasmBase64"] = graphql::JVal(
      core::base64Encode(Bytes(reinterpret_cast<const std::uint8_t*>(wasm.data()), wasm.size())));
  a["sizeBytes"] = graphql::JVal(static_cast<std::int64_t>(wasm.size()));
  a["capabilitySummaryJson"] = graphql::JVal(summary);
  a["capabilityHash"] = graphql::JVal(std::string("h1"));
  a["tickIntervalMs"] = graphql::JVal(std::int64_t{250});
  a["fuelPerDispatch"] = graphql::JVal(fuel);
  a["abiVersion"] = graphql::JVal(abi);
  return a.dump();
}

void testClientArtifactBytes() {
  auto http = std::make_shared<RecordingHttp>();
  ExecAPI exec(clientOver(http), std::make_shared<FakeTransport>());
  const std::string wasm("\0asm\1\0\0\0", 8);
  const std::string digest = execSha256Hex(wasm);

  // The digest is hex either way round; the result carries it lowercase.
  http->clientArtifact = servedArtifact(wasm, upperHex(digest));
  const ExecModClientArtifactBytes a = exec.modClientArtifactBytes("77", "900");
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecModClientArtifact"));
  CHECK_EQ(http->requests.back()["variables"]["modId"].asString(), std::string("900"));
  CHECK_EQ(std::string(a.bytes.begin(), a.bytes.end()), wasm);
  CHECK_EQ(a.digest, digest);
  CHECK_EQ(a.modId, std::string("900"));
  CHECK_EQ(a.name, std::string("turret"));
  CHECK_EQ(a.gridId, std::string("5"));
  CHECK_EQ(a.clientVersion, 2);
  CHECK_EQ(a.sizeBytes, 8);
  CHECK_EQ(a.fuelPerDispatch, std::string("100000000"));
  CHECK_EQ(a.tickIntervalMs, 250);
  CHECK_EQ(a.capabilityHash, std::string("h1"));
  CHECK_EQ(a.abiVersion, kExecClientAbiVersion);
  CHECK_EQ(a.capabilitySummaryJson, std::string(kSummary));
  CHECK_EQ(a.capabilitySummary.hostFunctions, std::vector<std::string>{"hud_set"});

  std::optional<graphql::GraphQLOutcome> outcome;
  ExecModClientArtifactBytes decoded;
  auto fetch = [&] {
    outcome.reset();
    decoded = {};
    decoded.modId = "untouched";
    exec.modClientArtifactBytesAsync("77", "900", [&](graphql::GraphQLOutcome o, ExecModClientArtifactBytes d) {
      outcome = std::move(o);
      decoded = std::move(d);
    });
    CHECK(outcome.has_value());
  };
  fetch();
  CHECK(outcome->ok());
  CHECK_EQ(decoded.digest, digest);
  CHECK_EQ(decoded.bytes.size(), 8u);

  // Each refusal reaches the async twin as a Protocol outcome with nothing decoded, and the
  // blocking call as a CrowdyProtocolError (an empty result without exceptions).
  auto refused = [&](std::string artifact, const std::string& why) {
    http->clientArtifact = std::move(artifact);
    fetch();
    CHECK(outcome->kind == graphql::GraphQLErrorKind::Protocol);
    CHECK(outcome->status.code == Errc::Malformed);
    if (outcome->errorMessage.find(why) == std::string::npos) {
      std::fprintf(stderr, "refusal \"%s\" does not say \"%s\"\n", outcome->errorMessage.c_str(), why.c_str());
      CHECK(false);
    }
    CHECK(decoded.bytes.empty() && decoded.modId.empty() && decoded.digest.empty());
#ifndef CROWDY_NO_EXCEPTIONS
    bool threw = false;
    try {
      exec.modClientArtifactBytes("77", "900");
    } catch (const graphql::CrowdyProtocolError& e) {
      threw = true;
      CHECK(std::string(e.what()) == outcome->errorMessage);
    }
    CHECK(threw);
#else
    CHECK(exec.modClientArtifactBytes("77", "900").bytes.empty());
#endif
  };
  // Bytes other than the ones the digest names are never handed to a runtime.
  refused(servedArtifact(wasm + "x", digest),
          "CLIENT half of mod 900: the module's SHA-256 is " + execSha256Hex(wasm + "x") + ", not the digest " +
              digest + " it was served with");
  refused(servedArtifact(wasm, digest, kSummary, 1), "CLIENT half of mod 900 is built for CLIENT ABI 1; this SDK runs ABI 0");
  // The ABI is checked first, as CrowdyJS checks it.
  refused(servedArtifact(wasm + "x", digest, "not json", 7), "built for CLIENT ABI 7");
  const std::string noBound = "CLIENT half of mod 900: its capability summary does not parse, so nothing bounds its host calls";
  refused(servedArtifact(wasm, digest, "not json"), noBound);
  refused(servedArtifact(wasm, digest, R"({"version":1,"target":"client"})"), noBound);
  refused(servedArtifact(wasm, digest, R"({"hostFunctions":["hud_set",1]})"), noBound);
  refused(servedArtifact(wasm, digest, kSummary, 0, "lots"), "its fuel per dispatch is not an integer");
  refused(R"({"modId":"900","digest":"ab","wasmBase64":"@@@@","abiVersion":0,"capabilitySummaryJson":"{\"hostFunctions\":[]}"})",
          "CLIENT half of mod 900: its module is not base64");
  refused(R"({"modId":"900","digest":"ab","abiVersion":0})", "execModClientArtifact returned no CLIENT module");
  refused("null", "execModClientArtifact returned no CLIENT module");

  // The API's refusals pass through untouched: every refusal to serve is NOT_FOUND.
  http->artifactNotFound = true;
  fetch();
  CHECK(outcome->kind == graphql::GraphQLErrorKind::GraphQL);
  CHECK_EQ(outcome->errors.at(0).code, std::string("NOT_FOUND"));
  CHECK(decoded.bytes.empty() && decoded.modId.empty());
#ifndef CROWDY_NO_EXCEPTIONS
  bool threw = false;
  try {
    exec.modClientArtifactBytes("77", "900");
  } catch (const graphql::CrowdyGraphQLError& e) {
    threw = true;
    CHECK_EQ(e.code(), std::string("NOT_FOUND"));
  }
  CHECK(threw);
#else
  CHECK(exec.modClientArtifactBytes("77", "900").bytes.empty());
#endif
}

#ifndef CROWDY_NO_EXCEPTIONS
struct RecordingClientRuntime final : studio::ICrowdyStudioClientRuntime {
  std::vector<studio::CrowdyStudioClientArtifact> started;
  int stopped = 0;
  void start(const studio::CrowdyStudioClientArtifact& artifact) override { started.push_back(artifact); }
  void stop() override { ++stopped; }
};

std::size_t countOp(const RecordingHttp& http, std::string_view op) {
  std::size_t n = 0;
  for (const auto& r : http.requests) n += r["operationName"].asString() == op ? 1 : 0;
  return n;
}

// Crowdy Studio's production runtime: the SERVER target as the grid's mod, the CLIENT target as its
// CLIENT half.
void testStudioModRuntime() {
  auto http = std::make_shared<RecordingHttp>();
  auto gql = std::make_shared<graphql::GraphQLClient>(graphql::GraphQLClientConfig{"http://test/graphql", 1000}, http,
                                                      std::make_shared<graphql::AuthState>());
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  gql->setDispatcher(dispatcher);
  auto transport = std::make_shared<FakeTransport>();
  ExecAPI exec(gql, transport);
  RecordingClientRuntime engine;
  studio::CrowdyStudioModRuntime runtime(exec, &engine, [dispatcher] { dispatcher->drain(); });
  const studio::CrowdyStudioProjectScope scope{"77", "5"};

  // The mod build takes only the crate's files, under a crate name a build accepts.
  studio::CrowdyStudioDeployTargetInput server;
  server.scope = scope;
  server.target = studio::CrowdyStudioTarget::Server;
  server.moduleName = "3d-server";
  server.projectId = "p1";
  for (const char* path : {"Cargo.toml", "README.md", "programs/sky.js", "src/lib.rs", "src/sky.rs"}) {
    studio::CrowdyStudioProjectFile file;
    file.target = studio::CrowdyStudioTarget::Server;
    file.path = path;
    file.content = "x";
    server.files.push_back(std::move(file));
  }
  CHECK_EQ(runtime.deploy(server).versionId, std::string("b1"));
  auto crate = http->requests.back()["variables"]["crate"];
  CHECK_EQ(crate["name"].asString(), std::string("mod-3d-server"));
  CHECK_EQ(crate["files"].size(), 4u);
  for (std::size_t i = 0; i < crate["files"].size(); ++i) {
    CHECK(crate["files"].at(i)["path"].asString() != "programs/sky.js");
  }

  // Building, then succeeded: the mod is deployed once, when the build first succeeds.
  CHECK_EQ(runtime.versions(scope, "3d-server").at(0).compileStatus, std::string("building"));
  CHECK_EQ(countOp(*http, "ExecModDeploy"), 0u);
  CHECK_EQ(runtime.versions(scope, "3d-server").at(0).compileStatus, std::string("succeeded"));
  CHECK_EQ(runtime.versions(scope, "3d-server").at(0).versionId, std::string("b1"));
  CHECK_EQ(countOp(*http, "ExecModDeploy"), 1u);
  runtime.setEnabled(scope, "3d-server", true);
  auto vars = http->requests.back()["variables"];
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecModSetEnabled"));
  CHECK_EQ(vars["name"].asString(), std::string("3d-server"));
  CHECK(vars["enabled"].asBool());

  const auto lines = runtime.logs(scope, "3d-server");
  CHECK_EQ(lines.size(), 2u);
  CHECK_EQ(lines.at(0).level, std::string("error"));
  CHECK_EQ(lines.at(0).text, std::string("boom"));
  CHECK_EQ(lines.at(1).level, std::string("info"));
  CHECK_EQ(http->requests.back()["variables"]["limit"].asInt64(), 50);

  // A name that is not a mod's is refused before anything is sent.
  const std::size_t sent = http->requests.size();
  server.moduleName = "Weather Server";
  bool refused = false;
  try {
    (void)runtime.deploy(server);
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  CHECK(refused);
  CHECK_EQ(http->requests.size(), sent);

  // The CLIENT target is the mod's CLIENT half: a crowdy-client-sdk crate built with modClientBuild.
  studio::CrowdyStudioDeployTargetInput client;
  client.scope = scope;
  client.target = studio::CrowdyStudioTarget::Client;
  client.moduleName = "3d-client";
  client.modName = "3d-server";
  client.projectId = "p1";
  for (const char* path : {"Cargo.toml", "src/lib.rs", "notes.txt"}) {
    studio::CrowdyStudioProjectFile file;
    file.target = studio::CrowdyStudioTarget::Client;
    file.path = path;
    file.content = std::string(path) == "Cargo.toml" ? "[dependencies]\ncrowdy-client-sdk = \"0.1.0\"\n" : "x";
    client.files.push_back(std::move(file));
  }
  CHECK_EQ(runtime.deploy(client).versionId, std::string("cb1"));
  CHECK_EQ(http->requests.back()["operationName"].asString(), std::string("ExecModClientBuild"));
  crate = http->requests.back()["variables"]["crate"];
  CHECK_EQ(crate["name"].asString(), std::string("mod-3d-client"));
  CHECK_EQ(crate["files"].size(), 2u);
  const auto clientVersions = runtime.versions(scope, "3d-client");
  CHECK_EQ(clientVersions.size(), 1u);
  CHECK_EQ(clientVersions.at(0).versionId, std::string("cb1"));

  // Running it attaches it to the project's mod, consents as its author, and starts the module
  // the API served, checked against its digest.
  const std::string wasm("\0asm\1\0\0\0", 8);
  http->clientDigest = execSha256Hex(wasm);
  http->clientArtifact = servedArtifact(wasm, http->clientDigest);
  runtime.startClient(scope, "3d-client", "cb1");
  const std::size_t attach = http->requests.size() - 3;
  CHECK_EQ(http->requests.at(attach)["operationName"].asString(), std::string("ExecModClientDeploy"));
  CHECK_EQ(http->requests.at(attach)["variables"]["name"].asString(), std::string("3d-server"));
  CHECK_EQ(http->requests.at(attach)["variables"]["buildId"].asString(), std::string("cb1"));
  CHECK_EQ(http->requests.at(attach + 1)["operationName"].asString(), std::string("ExecConsentClientMod"));
  CHECK_EQ(http->requests.at(attach + 1)["variables"]["capabilityHash"].asString(), std::string("h1"));
  CHECK_EQ(http->requests.at(attach + 2)["operationName"].asString(), std::string("ExecModClientArtifact"));
  CHECK_EQ(engine.started.size(), 1u);
  CHECK_EQ(engine.started.at(0).modName, std::string("3d-server"));
  CHECK_EQ(engine.started.at(0).versionId, std::string("cb1"));
  CHECK_EQ(std::string(engine.started.at(0).module.bytes.begin(), engine.started.at(0).module.bytes.end()), wasm);
  CHECK_EQ(engine.started.at(0).module.capabilitySummary.hostFunctions, std::vector<std::string>{"hud_set"});

  // A module other than the one just attached is not started.
  http->clientDigest = std::string(64, 'f');
  bool mismatch = false;
  try {
    runtime.startClient(scope, "3d-client", "cb1");
  } catch (const std::runtime_error& error) {
    mismatch = std::string(error.what()) == "The served CLIENT half is not the one just attached; deploy again";
  }
  CHECK(mismatch);
  CHECK_EQ(engine.started.size(), 1u);

  // A preview the API does not serve says why.
  http->artifactNotFound = true;
  bool unserved = false;
  try {
    runtime.startClient(scope, "3d-client", "cb1");
  } catch (const std::runtime_error& error) {
    unserved = std::string(error.what()).find("3d-client is attached to mod '3d-server', but its preview did not load") == 0;
  }
  CHECK(unserved);
  http->artifactNotFound = false;
  runtime.stopClient();
  CHECK_EQ(engine.stopped, 1);

  // A crate on legacy player compute is refused with CrowdyJS 18's words, before anything is sent.
  studio::CrowdyStudioDeployTargetInput legacy = client;
  legacy.files.at(0).content = "[dependencies]\n  crowdy-compute-sdk = \"0.2\"\n";
  const std::size_t beforeLegacy = http->requests.size();
  bool refusedLegacy = false;
  try {
    (void)runtime.deploy(legacy);
  } catch (const std::invalid_argument& error) {
    refusedLegacy = std::string_view(error.what()) == studio::kCrowdyStudioLegacyClientCrate;
  }
  CHECK(refusedLegacy);
  CHECK_EQ(http->requests.size(), beforeLegacy);
  CHECK(!studio::crowdyStudioIsLegacyClientCrate("crowdy-client-sdk = \"0.1.0\"\n# crowdy-compute-sdk = \"0.2\""));

  // A CLIENT-only project's CLIENT half rides the mod named for its CLIENT module: with none of
  // the player's on the grid, the mod starter is deployed under it and switched on first.
  studio::CrowdyStudioDeployTargetInput hud = client;
  hud.moduleName = "hud";
  hud.modName = "hud";
  hud.clientOnly = true;
  CHECK_EQ(runtime.deploy(hud).versionId, std::string("cb1"));
  http->clientDigest = execSha256Hex(wasm);
  const std::size_t beforeHud = http->requests.size();
  runtime.startClient(scope, "hud", "cb1");
  std::vector<std::string> hudOps;
  for (std::size_t i = beforeHud; i < http->requests.size(); ++i) {
    hudOps.push_back(http->requests.at(i)["operationName"].asString());
  }
  const std::vector<std::string> expectedHud = {"ExecMyMods",          "ExecModStarter",      "ExecModBuild",
                                                "ExecModBuildStatus",  "ExecModDeploy",       "ExecModSetEnabled",
                                                "ExecModClientDeploy", "ExecConsentClientMod", "ExecModClientArtifact"};
  CHECK_EQ(hudOps, expectedHud);
  CHECK_EQ(http->requests.at(beforeHud + 2)["variables"]["crate"]["files"].size(), 2u);
  CHECK_EQ(http->requests.at(beforeHud + 4)["variables"]["name"].asString(), std::string("hud"));
  CHECK_EQ(engine.started.back().modName, std::string("hud"));

  // A CLIENT-only project's CLIENT module name must be a mod's.
  hud.moduleName = hud.modName = "Hud Client";
  bool badName = false;
  try {
    (void)runtime.deploy(hud);
  } catch (const std::invalid_argument& error) {
    badName = std::string(error.what()).find("names the mod its CLIENT half rides") != std::string::npos;
  }
  CHECK(badName);

  // Invoke: one exec connection to mod:<name> on the grid; the reply decoded as JSON.
  std::thread gateway([&] {
    if (!until(*dispatcher, [&] { return transport->count() == 1; })) return;
    auto conn = transport->at(0);
    conn->open();
    for (const auto* status : {"ok", "refused"}) {
      std::optional<Sent> call;
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < end) {
        for (const auto& frame : conn->frames()) {
          Sent parsed = parse(frame);
          if (parsed.tag == 0x01 && (!call || parsed.rid > call->rid)) call = parsed;
        }
        if (call && (std::string_view(status) == "ok" || call->name == "state")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (!call) return;
      if (std::string_view(status) == "ok") {
        CHECK_EQ(call->nodeType, std::string("mod:3d-server"));
        CHECK_EQ(call->key, std::string("5"));
        CHECK_EQ(call->name, std::string("visit"));
        CHECK_EQ(graphql::Json::fromMsgpack(call->payload)["x"].asInt64(), 1);
        conn->deliver(reply(call->rid, 0, graphql::Json::parse(R"({"visits":2})").toMsgpack()));
      } else {
        conn->deliver(reply(call->rid, 1, "nope"));
      }
    }
  });
  const auto invoked = runtime.invoke(scope, "3d-server", "visit", std::string(R"({"x":1})"));
  CHECK_EQ(invoked.resultJson, std::string(R"({"visits":2})"));
  bool appError = false;
  try {
    (void)runtime.invoke(scope, "3d-server", "state", std::nullopt);
  } catch (const std::runtime_error& error) {
    appError = std::string(error.what()) == "AppError: nope";
  }
  gateway.join();
  CHECK(appError);
  CHECK_EQ(transport->count(), 1u);
}
#endif

// ---- where a connect token may go, and what a gateway's refusal says ----

void testGatewayRefusal() {
  // The cases are CrowdyJS's (test/unit/fixtures/exec-gateway-cases.json): both SDKs answer alike.
  std::ifstream in(std::string(CROWDY_PARITY_FIXTURE_DIR) + "/exec-gateway-cases.json");
  std::stringstream text;
  text << in.rdbuf();
  graphql::Json fx = graphql::Json::parse(text.str());
  CHECK(fx["cases"].size() >= 15);
  fx["cases"].forEach([](graphql::Json c) {
    const auto why = execGatewayRefusal(c["gameApi"].asString(), c["gateway"].asString());
    if (why.has_value() == c["dials"].asBool()) {
      std::fprintf(stderr, "%s: %s -> %s (%s)\n", c["note"].asString().c_str(), c["gameApi"].asString().c_str(),
                   c["gateway"].asString().c_str(), why.value_or("dials").c_str());
    }
    CHECK(why.has_value() != c["dials"].asBool());
  });
  CHECK_EQ(execGatewayRefusal("https://ck.dev.crowdedkingdoms.com/graphql", "ws://ckx-or-1.exec.dev.crowdedkingdoms.com")
               .value_or(""),
           std::string("a game API on https: hands out wss: gateways only"));
  CHECK(execGatewayRefusal("https://ck.dev.crowdedkingdoms.com/graphql", "wss://gw.example.org")
            .value_or("")
            .rfind("gw.example.org is outside the estate of ck.dev.crowdedkingdoms.com", 0) == 0);

  // Through the Game API: a gateway the check refuses is never dialed, and the attempt says why.
  auto http = std::make_shared<RecordingHttp>();
  http->gatewayUrl = "ws://ckx-or-1.exec.dev.crowdedkingdoms.com";
  auto gql = std::make_shared<graphql::GraphQLClient>(
      graphql::GraphQLClientConfig{"https://ck.dev.crowdedkingdoms.com/graphql", 1000}, http,
      std::make_shared<graphql::AuthState>());
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  gql->setDispatcher(dispatcher);
  auto transport = std::make_shared<FakeTransport>();
  ExecAPI exec(gql, transport);
  std::optional<Errc> failed;
  exec.connectAsync("77", {}, [&](Result<std::shared_ptr<ExecConnection>> r) { failed = r.error(); });
  CHECK(until(*dispatcher, [&] { return failed.has_value(); }));
  CHECK(*failed == Errc::NotConnected);
  const std::string why =
      "refusing the gateway ws://ckx-or-1.exec.dev.crowdedkingdoms.com: a game API on https: hands out wss: gateways only";
  for (const bool developer : {false, true}) {
    auto conn = developer ? exec.connectAsDeveloper("77") : exec.connect("77");
    std::optional<ExecReply> refused;
    conn->callRaw("arena", "m1", "state", "", [&](ExecReply r) { refused = r; });
    CHECK(until(*dispatcher, [&] { return refused.has_value(); }));
    CHECK(refused->status == ExecStatus::Unavailable);
    CHECK_EQ(refused->message(), why);
    CHECK(conn->lastFailure().has_value());
    CHECK_EQ(conn->lastFailure()->message(), why);
    conn->close();
  }
  CHECK_EQ(transport->count(), 0u);

  // A gateway on the Game API's estate is dialed.
  http->gatewayUrl = "wss://ckx-or-1.exec.dev.crowdedkingdoms.com";
  auto conn = exec.connect("77");
  CHECK(until(*dispatcher, [&] { return transport->count() == 1; }));
  CHECK_EQ(transport->at(0)->url, std::string("wss://ckx-or-1.exec.dev.crowdedkingdoms.com/v1/connect?token=t1"));
  conn->close();
}

WebSocketEvent refusedUpgrade(int status, std::string body) {
  WebSocketEvent ev;
  ev.kind = WebSocketEventKind::Error;
  ev.error.message = "WebSocket handshake failed: Refused WebSockets upgrade: " + std::to_string(status);
  ev.error.httpStatus = status;
  ev.error.httpBody = std::move(body);
  return ev;
}

void testRefusedUpgrade() {
  ExecConnectOptions opts;
  opts.callTimeoutMs = 2000;
  struct Case {
    int status;
    std::string body;
    ExecStatus expected;
    Errc waiter;
    std::string message;
  };
  const std::vector<Case> cases = {
      // ck-exec 0.10.0+: a refused connect token, with the reason as the body.
      {401, "token expired\n", ExecStatus::Denied, Errc::Rejected,
       "the gateway refused the connection (HTTP 401: token expired)"},
      // libcurl reports the status alone.
      {401, "", ExecStatus::Denied, Errc::Rejected, "the gateway refused the connection (HTTP 401)"},
      // A player past their session cap: retryable, so Unavailable, with the reason.
      {429, "a player may hold 16 sessions to an app", ExecStatus::Unavailable, Errc::NotConnected,
       "the gateway refused the connection (HTTP 429: a player may hold 16 sessions to an app)"},
  };
  for (const auto& c : cases) {
    auto transport = std::make_shared<FakeTransport>();
    auto dispatcher = std::make_shared<graphql::Dispatcher>();
    auto conn = ExecConnection::open(transport, dispatcher, ExecEndpoint{"ws://127.0.0.1:7721", "stale", "h"}, opts);
    std::optional<Status> opened;
    conn->connect([&](Status s) { opened = s; });
    std::optional<ExecReply> r;
    conn->callRaw("arena", "m1", "state", "", [&](ExecReply x) { r = x; });
    CHECK(until(*dispatcher, [&] { return transport->count() == 1; }));
    transport->at(0)->emit(refusedUpgrade(c.status, c.body));
    CHECK(until(*dispatcher, [&] { return r.has_value() && opened.has_value(); }));
    CHECK(r->status == c.expected);
    CHECK_EQ(r->message(), c.message);
    CHECK(opened->code == c.waiter);
    CHECK(conn->lastFailure().has_value() && conn->lastFailure()->status == c.expected);
    CHECK_EQ(conn->lastFailure()->message(), c.message);
    conn->close();
  }

  // A gateway before 0.10.0 upgraded, then closed 4401: a call in flight is Denied too.
  auto transport = std::make_shared<FakeTransport>();
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  auto conn = ExecConnection::open(transport, dispatcher, ExecEndpoint{"ws://127.0.0.1:7721", "stale", "h"}, opts);
  CHECK(until(*dispatcher, [&] { return transport->count() == 1; }));
  transport->at(0)->open();
  std::optional<ExecReply> r;
  conn->callRaw("arena", "m1", "state", "", [&](ExecReply x) { r = x; });
  CHECK(until(*dispatcher, [&] { return last(transport->at(0), 0x01).has_value(); }));
  CHECK(!conn->lastFailure().has_value());
  WebSocketEvent closed;
  closed.kind = WebSocketEventKind::Close;
  closed.close.code = 4401;
  closed.close.reason = "token expired";
  transport->at(0)->emit(closed);
  CHECK(until(*dispatcher, [&] { return r.has_value(); }));
  CHECK(r->status == ExecStatus::Denied);
  CHECK_EQ(r->message(), std::string("token expired"));
  CHECK(conn->lastFailure().has_value() && conn->lastFailure()->status == ExecStatus::Denied);
  conn->close();
}

int main() {
  testGoldenFrames();
  testStatusesAndDigests();
  testConnection();
  testCallTimesOut();
  testGatewayRefusal();
  testRefusedUpgrade();
  testOperations();
  testBuilds();
  testMods();
#ifndef CROWDY_NO_EXCEPTIONS
  testStudioModRuntime();
#endif
  testClientHalves();
  testCapabilitySummary();
  testClientArtifactBytes();
  std::puts("exec_test OK");
  return 0;
}
