// What it costs a player's Connection to receive: datagram to handler, in nanoseconds a
// notification, with the socket read and the HMAC verification on one side (pump) and the
// dispatch to the handler on the other (poll).
//
// Each round a loopback "server" socket puts a fixed number of datagrams into the
// connection's receive buffer, and only then is the clock started: the round times the
// connection draining them, with no sender running beside it. A datagram is what the
// realtime servers send a session that has not advertised signed bundles: a plain
// MESSAGE_BUNDLE of six actor-update notifications, each with its own HMAC, so the work is
// six verifications a datagram. With `signed` it is one MESSAGE_BUNDLE_SIGNED of the same six
// without their own HMACs: one verification a datagram.
//
// Run pinned to a CPU and more than once; the numbers move a few percent between runs:
//
//   taskset -c 3 ./bench_receive            # 300 rounds of 1,000 datagrams, six HMACs each
//   taskset -c 3 ./bench_receive signed     # one HMAC a datagram
//   taskset -c 3 ./bench_receive plain 500 800
//
// It uses nothing newer than 0.93.0, so the same file measures an older tree.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "crowdy/core/crypto.hpp"
#include "crowdy/replication/connection.hpp"
#include "crowdy/wire/codec.hpp"

using namespace crowdy;
using namespace crowdy::replication;
using Clock = std::chrono::steady_clock;

namespace {

const std::string kToken(64, 'k');
constexpr std::int64_t kAppId = 7;
constexpr int kPerDatagram = 6;

struct Provider final : ISessionProvider {
  int port;
  explicit Provider(int p) : port(p) {}
  Result<Assignment> assignServer() override { return Assignment{"127.0.0.1", "", port}; }
  Result<TokenInfo> refreshToken() override { return TokenInfo{kToken, 42, 0}; }
};

void check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "bench_receive: %s\n", what);
  std::exit(1);
}

/// One signed actor-update notification, 64 bytes of payload.
std::vector<std::uint8_t> notification(std::uint8_t sequence) {
  wire::LongSpatialParams p;
  p.type = wire::MessageType::ActorUpdateNotification;
  p.appId = kAppId;
  p.chunk = {1, 2, 3};
  p.distance = 8;
  std::memset(p.uuid.data(), 'b', 32);
  std::uint8_t payload[64] = {1, 2, 3};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = 1'700'000'000'000LL;
  p.sequence = sequence;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(sizeof(payload)));
  const auto n = wire::encodeLongSpatial(core::opensslCrypto(), p, *wire::Token64::fromString(kToken),
                                         MutableBytes(buf.data(), buf.size()));
  check(n.ok(), "encode failed");
  buf.resize(n.value());
  return buf;
}

/// [type][len LE16][member]...; a signed bundle ends with one HMAC over all of it, and its
/// members carry none of their own (containsAuth 0).
std::vector<std::uint8_t> bundle(bool signedBundle) {
  std::vector<std::uint8_t> dg = {static_cast<std::uint8_t>(
      signedBundle ? wire::MessageType::MessageBundleSigned : wire::MessageType::MessageBundle)};
  for (int i = 0; i < kPerDatagram; ++i) {
    std::vector<std::uint8_t> m = notification(static_cast<std::uint8_t>(i));
    if (signedBundle) {
      m[wire::offsets::kContainsAuth] = 0;
      const std::size_t hmacAt = m.size() - wire::kTailWithHmac;
      m.erase(m.begin() + static_cast<std::ptrdiff_t>(hmacAt),
              m.begin() + static_cast<std::ptrdiff_t>(hmacAt + wire::kHmacTagSize));
    }
    dg.push_back(static_cast<std::uint8_t>(m.size() & 0xff));
    dg.push_back(static_cast<std::uint8_t>(m.size() >> 8));
    dg.insert(dg.end(), m.begin(), m.end());
  }
  if (signedBundle) {
    const std::size_t body = dg.size();
    dg.resize(body + wire::kHmacTagSize);
    check(wire::spatialHmac(core::opensslCrypto(), Bytes(dg.data(), body), *wire::Token64::fromString(kToken),
                            dg.data() + body),
          "bundle HMAC failed");
  }
  return dg;
}

double percentile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
}

}  // namespace

int main(int argc, char** argv) {
  const bool signedBundle = argc > 1 && std::string(argv[1]) == "signed";
  const int rounds = argc > 2 ? std::atoi(argv[2]) : 300;
  const int perRound = argc > 3 ? std::atoi(argv[3]) : 1000;
  check(rounds > 0 && perRound > 0, "usage: bench_receive [plain|signed] [rounds] [datagrams a round]");

  const int server = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  check(server >= 0, "socket");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
  socklen_t len = sizeof(addr);
  check(::getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");

  Config cfg;
  cfg.appId = kAppId;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;
  cfg.bundleSends = false;
  cfg.socketRecvBufferBytes = 4 << 20;
  cfg.ringCapacity = 1 << 15;
  check(static_cast<std::size_t>(perRound) * kPerDatagram < cfg.ringCapacity, "a round does not fit the event ring");
  Connection conn(cfg, std::make_shared<Provider>(ntohs(addr.sin_port)), core::opensslCrypto());
  std::uint64_t delivered = 0;
  Handlers handlers;
  handlers.actorUpdate = [&delivered](const SpatialNotification&) { ++delivered; };
  conn.setHandlers(std::move(handlers));
  check(conn.connect().ok(), "connect");
  conn.pump(0);
  core::ActorUuid me;
  std::memset(me.data(), 'a', 32);
  check(conn.sendHeartbeat({0, 0, 0}, me).ok(), "heartbeat");

  // The client's address, from the first thing it sends.
  sockaddr_in client{};
  socklen_t clientLen = sizeof(client);
  std::uint8_t scratch[2048];
  check(::recvfrom(server, scratch, sizeof(scratch), 0, reinterpret_cast<sockaddr*>(&client), &clientLen) > 0,
        "the connection sent nothing");

  const std::vector<std::uint8_t> datagram = bundle(signedBundle);
  const std::uint64_t perRoundNotifications = static_cast<std::uint64_t>(perRound) * kPerDatagram;
  std::vector<double> pumpNs, pollNs;
  const int warmUp = std::max(10, rounds / 10);
  for (int round = -warmUp; round < rounds; ++round) {
    for (int i = 0; i < perRound; ++i) {
      check(::sendto(server, datagram.data(), datagram.size(), 0, reinterpret_cast<sockaddr*>(&client),
                     clientLen) == static_cast<ssize_t>(datagram.size()),
            "sendto");
    }
    // Everything is in the connection's receive buffer (a loopback send queues it before it
    // returns). Read and verify it all, then hand it all to the handler.
    const std::uint64_t before = delivered;
    std::size_t read = 0;
    const auto t0 = Clock::now();
    while (read < static_cast<std::size_t>(perRound)) {
      const std::size_t n = conn.pump(0);
      check(n > 0, "a datagram was lost: raise net.core.rmem_max or lower the datagrams a round");
      read += n;
    }
    const auto t1 = Clock::now();
    while (delivered - before < perRoundNotifications) {
      check(conn.poll() > 0, "a notification was not delivered");
    }
    const auto t2 = Clock::now();
    if (round < 0) continue;
    const auto per = [&](Clock::time_point a, Clock::time_point b) {
      return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) /
             static_cast<double>(perRoundNotifications);
    };
    pumpNs.push_back(per(t0, t1));
    pollNs.push_back(per(t1, t2));
  }
  const auto stats = conn.stats();
  check(stats.hmacFailures == 0 && stats.ringDropped == 0 && stats.malformed == 0, "the connection dropped something");
  conn.disconnect();
  ::close(server);

  // The median says what a round usually took. The fastest round is the one nothing else on
  // the machine interrupted, so it is the number to compare two builds by on a busy machine.
  const double pump = percentile(pumpNs, 0.5), poll = percentile(pollNs, 0.5);
  const double pumpBest = percentile(pumpNs, 0.0), pollBest = percentile(pollNs, 0.0);
  std::printf("%s bundles of %d, %d rounds of %d datagrams, ns a notification\n"
              "  median:  pump %6.1f  poll %6.1f  both %6.1f  = %8.0f notifications/s\n"
              "  fastest: pump %6.1f  poll %6.1f  both %6.1f  = %8.0f notifications/s\n",
              signedBundle ? "signed" : "plain", kPerDatagram, rounds, perRound, pump, poll, pump + poll,
              1e9 / (pump + poll), pumpBest, pollBest, pumpBest + pollBest, 1e9 / (pumpBest + pollBest));
  return 0;
}
