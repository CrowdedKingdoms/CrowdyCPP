// Optional live ck-exec evidence over the real curl WebSocket transport: calls,
// a subscription's pushes, a platform method refused, and a ping, against a
// gateway running the ck-exec demo app (lobby, arena, mobs hubs).
//
//   CROWDY_E2E_EXEC_GATEWAY  e.g. ws://127.0.0.1:7721 or wss://<host>
//   CROWDY_E2E_EXEC_TOKEN    a connect token for that host (execConnect's `token`)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/websocket.hpp"

using namespace crowdy;
using namespace crowdy::domains;

namespace {

int fail(const char* what) {
  std::fprintf(stderr, "e2e_exec_gateway: %s\n", what);
  return 1;
}

template <typename Fn>
bool until(graphql::Dispatcher& d, Fn&& done, int ms = 10000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    d.drain();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  d.drain();
  return done();
}

}  // namespace

int main() {
  const char* gateway = std::getenv("CROWDY_E2E_EXEC_GATEWAY");
  const char* token = std::getenv("CROWDY_E2E_EXEC_TOKEN");
  if (!gateway || !token || !*gateway || !*token) {
    std::puts("CROWDY_E2E_EXEC_GATEWAY / CROWDY_E2E_EXEC_TOKEN unset; skipping");
    return 77;
  }
  auto transport = graphql::makeCurlWebSocketTransport();
  if (!transport) {
    std::puts("built without a curl WebSocket transport; skipping");
    return 77;
  }
  auto dispatcher = std::make_shared<graphql::Dispatcher>();
  auto conn = ExecConnection::open(transport, dispatcher, ExecEndpoint{gateway, token, ""});

  std::optional<ExecReply> opened;
  conn->call("lobby", "", "open_match", graphql::JVal(), [&](ExecReply r) { opened = std::move(r); });
  if (!until(*dispatcher, [&] { return opened.has_value(); }) || !opened->ok()) return fail("lobby.open_match");
  std::printf("lobby.open_match -> %s\n", opened->value().dump().c_str());

  int pushes = 0;
  std::optional<ExecReply> subscribed;
  conn->subscribe("mobs", "m1", "mobs", [&](const ExecPush& p) {
    ++pushes;
    std::printf("push %s/%s %s: ticks %lld\n", p.nodeType.c_str(), p.key.c_str(), p.topic.c_str(),
                static_cast<long long>(p.value()["ticks"].asInt64()));
  }, [&](ExecReply r) { subscribed = std::move(r); });
  if (!until(*dispatcher, [&] { return subscribed.has_value(); }) || !subscribed->ok()) return fail("subscribe mobs");

  std::optional<ExecReply> state;
  conn->call("mobs", "m1", "state", graphql::JVal(), [&](ExecReply r) { state = std::move(r); });
  if (!until(*dispatcher, [&] { return state.has_value(); }) || !state->ok()) return fail("mobs.state");
  if (state->value()["timers"].at(0).asString() != "tick") return fail("the mobs hub has its tick timer");
  if (state->value()["watching"].size() != 1) return fail("subscribing joined the mobs hub");

  std::optional<ExecReply> refused;
  conn->call("mobs", "m1", "$timer", graphql::JVal(), [&](ExecReply r) { refused = std::move(r); });
  if (!until(*dispatcher, [&] { return refused.has_value(); }) || refused->status != ExecStatus::Denied) {
    return fail("a client's $timer is refused");
  }

  if (!until(*dispatcher, [&] { return pushes >= 1; }, 5000)) return fail("the ticking hub pushes");

  std::optional<ExecReply> pong;
  conn->ping([&](ExecReply r) { pong = std::move(r); });
  if (!until(*dispatcher, [&] { return pong.has_value(); }) || !pong->ok()) return fail("ping");

  conn->close();
  std::puts("e2e_exec_gateway OK");
  return 0;
}
