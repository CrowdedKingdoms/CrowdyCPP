// Offline FleetSession test, in replication_test's manner: a fake in-process UDP "server" on
// 127.0.0.1 receives what the session sends, checks it byte for byte, and answers with the
// fleet session's server-to-client messages. Manual-pump mode on a clock the test moves,
// except the last case, which runs the owned network thread.
//
// NO STEP DEPENDS ON HOW LONG A DATAGRAM TAKES TO CROSS THE LOOPBACK. Linux hands a loopback
// datagram to its receiver before sendto() returns; macOS does not, and a step that read
// "what has arrived by now" failed there now and then. A fake server knows the session it
// serves (FakeServer::serves), and each direction waits, in real time and with the test's
// clock standing still, on a count:
//
//  - Session to server: the session counts the datagrams it has sent (Stats::datagramsSent).
//    Every read first takes that many off the sockets, however long they take, and hands
//    them over in the order they left: the order of the sequence number each one ends with,
//    which one sending thread gives out as it sends. So "everything it sent" is exact, and
//    "nothing" means the count did not move.
//  - Server to session: FakeServer::send returns once the session has received the datagram
//    (Stats::datagramsReceived), pumping the session meanwhile.
//
// To see it hold, give the loopback a delay in a network namespace of its own (never on the
// host's):
//
//   sudo ip netns add fst && sudo ip netns exec fst ip link set lo up
//   sudo ip netns exec fst tc qdisc add dev lo root netem delay 5ms
//   sudo ip netns exec fst sudo -u "$USER" ./tests/fleet_session_test
//   sudo ip netns del fst
//
// A loopback with netem on it does not always keep the order. With jitter (`netem delay 3ms
// 2ms`) it never does. With a plain delay it does on one CPU, and on several it hands a
// datagram that is due to whichever CPU takes it first (measured at 2 ms on eight cores:
// about 300 of 140,000 datagrams of one thread arrived behind a later one). Every case holds
// under that but runSendsInFlightSwitchWithTheCarry, which judges by the order of arrival
// from two sending threads and says why at its head. Run the binary on one CPU (`taskset -c
// 0`) for a plain delay, and leave that case out (FLEET_TEST_LOOPBACK_REORDERS=1) for jitter.
//
// The golden bytes are wire_test's, computed independently (Python hmac/hashlib) from the
// wire as built (realtime servers v0.50.1; the build contract's sections 8 and 9).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/replication/fleet_session.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

const std::string kFleetToken = "fleet-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP";
const std::string kPlayerToken = "player-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO";
constexpr std::int64_t kFleetId = 9001;
constexpr std::int64_t kPlayerId = 4242;
constexpr std::int64_t kAppId = 7;
constexpr std::int64_t kEpochMs = 1795000000000LL;

// FLEET_CARRY (49) for the player above at kEpochMs. The vector's last byte is sequence 0;
// a session's capabilities take that number, so its first carry is the vector with 1 there
// (the sequence is after the HMAC and is not signed).
const char kGoldenFleetCarry[] =
    "3107000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000009210000000000000005e56eea10100005602abb9"
    "300581f6a20e247ea0b406f690646a3d27f9427e95c5f6363115b35a9d69b5b07c19a2450e61725d9995c19c"
    "940e41fa602fcce2535e780e5d2d81e3292300000000000000";
// ACTOR_UPDATE_REQUEST (128) for the carried player, sequence 1: chunk (1,-2,3), distance 8,
// uuid "0123456789abcdef0123456789abcdef", payload de ad be ef.
const char kGoldenCarriedActorUpdate[] =
    "8007000000000000000100000000000000feffffffffffffff03000000000000000800013031323334353637"
    "383961626364656630313233343536373839616263646566deadbeefafca3c1e53f866c6384fb186e2396664"
    "b62d7acd0fee6d9445b96e283d1c09f4921000000000000001";
// FLEET_RELEASE (50) for the player, sequence 2.
const char kGoldenFleetRelease[] =
    "3207000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000009210000000000000903cf330c14c7344619b97d3"
    "736c97d042c2669c9c8c1ed8e4b1922c327f684d292300000000000002";

const core::ICrypto& crypto() { return core::opensslCrypto(); }
wire::Token64 fleetKey() { return *wire::Token64::fromString(kFleetToken); }
wire::Token64 playerKey() { return *wire::Token64::fromString(kPlayerToken); }

/// A second player's token: 64 characters ending in the id's last digit.
std::string tokenFor(std::int64_t id) {
  std::string t(64, 'p');
  t[63] = static_cast<char>('0' + id % 10);
  t[62] = static_cast<char>('0' + (id / 10) % 10);
  return t;
}

std::string toHex(const std::vector<std::uint8_t>& data) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::uint8_t b : data) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xf]);
  }
  return out;
}

core::ActorUuid uuidOf(const char* s) {
  core::ActorUuid u;
  std::memcpy(u.data(), s, 32);
  return u;
}

core::ActorUuid uuid(char fill) {
  core::ActorUuid u;
  std::memset(u.data(), fill, 32);
  return u;
}

struct FakeServer;

/// Who pumps a session under test: the test (manual pump), or the session's own network thread.
enum class Pumped { ByTheTest, ByItsOwnThread };

/// What the fake servers know of one session under test: how many of its datagrams they have
/// taken off their sockets, and where in the session's sending order the latest one was.
struct Wire {
  FleetSession* session = nullptr;
  Pumped pumped = Pumped::ByTheTest;
  std::uint64_t read = 0;
  /// The place, in the order the session sent them, of the latest datagram read. A datagram
  /// ends with a one-byte sequence number that the session counts up by one for each.
  std::int64_t latest = -1;
  std::vector<FakeServer*> servers;

  /// The place of a datagram whose sequence number is `sequence`: the number nearest the one
  /// after the latest that ends in that byte. Datagrams that overtook each other on the way
  /// (fewer than 128 places apart) get the places they left in.
  std::int64_t placeOf(std::uint8_t sequence) {
    const std::int64_t next = latest + 1;
    const auto ahead = static_cast<std::int8_t>(static_cast<std::uint8_t>(sequence - static_cast<std::uint8_t>(next)));
    const std::int64_t place = next + ahead;
    if (place > latest) latest = place;
    return place;
  }
};

std::map<const FleetSession*, Wire>& wires() {
  static std::map<const FleetSession*, Wire> all;
  return all;
}

struct FakeServer {
  int fd = -1;
  int port = 0;
  sockaddr_in lastClient{};
  socklen_t lastClientLen = 0;
  /// The session this server serves, once serves() said so. Without one the server reads and
  /// sends as a bare socket does (a plain Connection's server, and the threaded cases).
  Wire* wire = nullptr;

  /// A datagram taken off the socket and not handed to the test yet.
  struct Arrival {
    std::int64_t place = 0;
    sockaddr_in from{};
    socklen_t fromLen = 0;
    std::vector<std::uint8_t> bytes;
  };
  std::vector<Arrival> inbox;  // in the order the session sent them
  std::int64_t arrivals = 0;

  void start() {
    fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    roomForABacklog();
  }

  ~FakeServer() {
    forget();
    if (fd >= 0) ::close(fd);
  }

  /// This server is one the session sends to and hears from. Say so before the session sends
  /// anything; a session that moves between two servers is served by both.
  void serves(FleetSession& session, Pumped pumped = Pumped::ByTheTest) {
    forget();
    Wire& w = wires()[&session];
    const bool unused = session.stats().datagramsSent == 0;
    if (w.servers.empty() || (unused && w.read != 0)) {
      // A session the servers have not met (or a new one where an earlier one stood).
      CHECK(unused);
      for (FakeServer* other : w.servers) other->wire = nullptr;
      w = Wire{};
      w.session = &session;
      w.pumped = pumped;
    }
    w.servers.push_back(this);
    wire = &w;
  }

  /// Back to a bare socket: what the session sends is read as it arrives, with no count.
  void forget() {
    if (!wire) return;
    auto& servers = wire->servers;
    servers.erase(std::remove(servers.begin(), servers.end(), this), servers.end());
    if (servers.empty()) wires().erase(wire->session);
    wire = nullptr;
  }

  /// A larger receive buffer than the system's default, which holds fewer than 300 small
  /// datagrams on Linux: a case that sends 256 before it reads (runVerifiedRefusals) or whose
  /// senders outrun the thread that reads would lose some to the kernel, and a datagram the
  /// kernel drops for want of room never arrives, which a count of what was sent cannot tell
  /// from one that is late. The largest of these the system allows is kept (macOS refuses a
  /// size above its limit).
  void roomForABacklog() {
    for (const int bytes : {8 << 20, 4 << 20, 2 << 20, 1 << 20}) {
      const auto* value = reinterpret_cast<const char*>(&bytes);
      if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, value, sizeof(bytes)) == 0) return;
    }
  }

  /// Take one datagram off the socket if one is there, into the inbox: at its place in the
  /// order the session sent them when the server serves a session, at the end otherwise.
  bool pull() {
    std::uint8_t buf[2048];
    Arrival got;
    got.fromLen = sizeof(got.from);
    const ssize_t n =
        ::recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&got.from), &got.fromLen);
    if (n <= 0) return false;
    got.bytes.assign(buf, buf + n);
    got.place = arrivals++;
    if (wire) {
      ++wire->read;
      got.place = wire->placeOf(got.bytes.back());
    }
    const auto after = std::upper_bound(inbox.begin(), inbox.end(), got.place,
                                        [](std::int64_t place, const Arrival& a) { return place < a.place; });
    inbox.insert(after, std::move(got));
    return true;
  }

  /// Wait, in real time, until a datagram is on the socket or `timeoutMs` passed.
  bool readable(int timeoutMs) const {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeoutMs) > 0;
  }

  /// Every datagram the session has sent by now is in an inbox when this returns, however
  /// long each took to arrive: the session's own count says how many there are.
  void gather() {
    if (!wire) return;
    const std::uint64_t sent = wire->session->stats().datagramsSent;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (wire->read < sent && std::chrono::steady_clock::now() < deadline) {
      bool any = false;
      for (FakeServer* server : wire->servers) {
        while (server->pull()) any = true;
      }
      if (!any) {
        std::vector<pollfd> fds;
        for (FakeServer* server : wire->servers) fds.push_back({server->fd, POLLIN, 0});
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 1);
      }
    }
    if (wire->read < sent) {
      std::fprintf(stderr, "the session sent %llu datagrams and its servers have read %llu\n",
                   static_cast<unsigned long long>(sent), static_cast<unsigned long long>(wire->read));
    }
    CHECK(wire->read >= sent);
  }

  /// The next datagram in the order the session sent them, or empty when there is none:
  /// everything the session has sent is waited for first, and when that is nothing, a
  /// datagram it sends within `timeoutMs` (a session with a thread of its own).
  std::vector<std::uint8_t> tryRecv(int timeoutMs) {
    gather();
    if (inbox.empty() && readable(timeoutMs)) {
      pull();
      gather();  // whatever left with it
    }
    if (inbox.empty()) return {};
    Arrival next = std::move(inbox.front());
    inbox.erase(inbox.begin());
    lastClient = next.from;
    lastClientLen = next.fromLen;
    return std::move(next.bytes);
  }

  std::vector<std::uint8_t> recvOne(int timeoutMs = 2000) {
    auto got = tryRecv(timeoutMs);
    CHECK(!got.empty());
    return got;
  }

  /// The session has sent nothing the test has not read: by its own count, and by 60 ms of
  /// the socket for anything the count would not know of.
  void expectNothing() { CHECK(tryRecv(60).empty()); }

  /// Everything the session has sent and the test has not read, in the order it left. A bare
  /// socket has no count to wait on: it reads until nothing has arrived for 40 ms.
  std::vector<std::vector<std::uint8_t>> takeAll() {
    std::vector<std::vector<std::uint8_t>> all;
    for (auto got = tryRecv(wire ? 0 : 40); !got.empty(); got = tryRecv(wire ? 0 : 40)) all.push_back(std::move(got));
    return all;
  }

  /// Put a datagram on the wire to the address the last one read came from, and wait for
  /// nothing: what send() does first, and all a bare socket can do.
  void sendUnheard(const std::vector<std::uint8_t>& data) {
    CHECK(lastClientLen > 0);
    CHECK(::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&lastClient),
                   lastClientLen) == static_cast<ssize_t>(data.size()));
  }

  /// Send the session a datagram, and return when the session has received it: its count of
  /// datagrams received moved. The test's clock stands still meanwhile. The session is pumped
  /// here when the test pumps it (what the datagram says is dispatched at the next poll(), as
  /// ever), and watched when it has a network thread of its own.
  void send(const std::vector<std::uint8_t>& data) {
    if (!wire) return sendUnheard(data);
    FleetSession& session = *wire->session;
    const std::uint64_t before = session.stats().datagramsReceived;
    sendUnheard(data);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.stats().datagramsReceived == before && std::chrono::steady_clock::now() < deadline) {
      if (wire->pumped == Pumped::ByTheTest) session.pump(1);
      else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(session.stats().datagramsReceived > before);
  }
};

/// A logger that keeps what is logged at WARN and above.
struct RecordingLogger final : core::ILogger {
  mutable std::mutex mutex;
  mutable std::vector<std::string> lines;
  bool enabled(core::LogLevel level) const override { return level >= core::LogLevel::Warn; }
  void log(core::LogLevel, std::string_view message) const override {
    std::lock_guard lock(mutex);
    lines.emplace_back(message);
  }
};

/// A clock that moves only when the test moves it.
struct ManualClock final : core::IClock {
  std::atomic<std::int64_t> epoch{kEpochMs};
  std::atomic<std::int64_t> mono{50'000};
  std::int64_t epochMillis() const override { return epoch.load(); }
  std::int64_t monotonicMillis() const override { return mono.load(); }
  void advance(std::int64_t ms) {
    epoch.fetch_add(ms);
    mono.fetch_add(ms);
  }
};

FleetConfig configFor(const FakeServer& server) {
  FleetConfig cfg;
  cfg.appId = kAppId;
  cfg.token = TokenInfo{kFleetToken, kFleetId, 0};
  cfg.server = Assignment{"127.0.0.1", "", server.port};
  cfg.transport.manualPump = true;
  cfg.transport.sessionReadyWaitMs = 0;
  // Each send is read straight off the wire: one message a datagram. The capabilities
  // leave once, when the session connects (a fleet session always sends them: they bind it
  // to its address), and no periodic one lands between the messages a case reads.
  cfg.transport.bundleSends = false;
  cfg.transport.advertiseIntervalMs = 3'600'000;
  // The silence timeout (on by default, and it holds the capabilities interval to a third of
  // itself) is off here: these cases read every datagram and move the clock by hand past a
  // server that says nothing. watchedConfigFor() has it as a FleetConfig starts with it.
  cfg.transport.watchdogSilenceMs = 0;
  return cfg;
}

/// configFor with the silence timeout and the capabilities interval a FleetConfig starts with.
FleetConfig watchedConfigFor(const FakeServer& server) {
  FleetConfig cfg = configFor(server);
  cfg.transport.watchdogSilenceMs = FleetConfig{}.transport.watchdogSilenceMs;
  cfg.transport.advertiseIntervalMs = Config{}.advertiseIntervalMs;
  return cfg;
}

// ---- what the realtime server sends a fleet session ----

std::vector<std::uint8_t> carryResultFrame(std::int64_t player, std::int64_t user, std::uint8_t status) {
  std::vector<std::uint8_t> f(wire::fleet::kCarryResultSize);
  f[0] = 51;
  le::writeI64(f.data() + 1, player);
  le::writeI64(f.data() + 9, user);
  f[17] = status;
  return f;
}

std::vector<std::uint8_t> releasedFrame(std::int64_t player, std::uint8_t reason) {
  std::vector<std::uint8_t> f(wire::fleet::kReleasedSize);
  f[0] = 52;
  le::writeI64(f.data() + 1, player);
  f[9] = reason;
  return f;
}

/// [53][count][ids][notification]; `count` is written as given, whatever `ids` holds.
std::vector<std::uint8_t> deliveryFrame(std::size_t count, const std::vector<std::int64_t>& ids,
                                        const std::vector<std::uint8_t>& notification) {
  std::vector<std::uint8_t> f(2 + ids.size() * 8 + notification.size());
  f[0] = 53;
  f[1] = static_cast<std::uint8_t>(count);
  for (std::size_t i = 0; i < ids.size(); ++i) le::writeI64(f.data() + 2 + i * 8, ids[i]);
  if (!notification.empty())
    std::memcpy(f.data() + 2 + ids.size() * 8, notification.data(), notification.size());
  return f;
}

std::vector<std::uint8_t> deliveryFrame(const std::vector<std::int64_t>& ids,
                                        const std::vector<std::uint8_t>& notification) {
  return deliveryFrame(ids.size(), ids, notification);
}

/// A signed server-to-client long-spatial notification (epoch millis in the tail slot).
std::vector<std::uint8_t> notification(wire::MessageType type, Bytes payload, std::uint8_t seq,
                                       const wire::Token64& key, std::int64_t appId = kAppId) {
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = appId;
  p.chunk = {1, 2, 3};
  p.distance = 8;
  p.uuid = uuid('b');
  p.payload = payload;
  p.gameTokenId = 1700000000000LL;
  p.sequence = seq;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(payload.size()));
  auto n = wire::encodeLongSpatial(crypto(), p, key, MutableBytes(buf.data(), buf.size()));
  CHECK(n.ok());
  buf.resize(n.value());
  return buf;
}

/// A 51, 52 or 53 as it travels alone or in a plain bundle: followed by its own HMAC,
/// HMAC-SHA256(key = `key`, message = the frame followed by the key's 64 octets).
std::vector<std::uint8_t> own(std::vector<std::uint8_t> frame, const wire::Token64& key) {
  const std::size_t len = frame.size();
  std::vector<std::uint8_t> message = frame;
  message.insert(message.end(), key.octets, key.octets + wire::kTokenOctets);
  frame.resize(len + wire::kHmacTagSize);
  CHECK(crypto().hmacSha256(key.bytes(), Bytes(message.data(), message.size()), frame.data() + len));
  return frame;
}
std::vector<std::uint8_t> own(std::vector<std::uint8_t> frame) { return own(std::move(frame), fleetKey()); }

/// The same notification in its unsigned form: containsAuth 0 and no HMAC of its own, as a
/// signed bundle's member is and as the notification inside a FLEET_DELIVERY always is.
std::vector<std::uint8_t> withoutOwnHmac(std::vector<std::uint8_t> note) {
  note[wire::offsets::kContainsAuth] = 0;
  const std::size_t hmacAt = note.size() - wire::kTailWithHmac;
  note.erase(note.begin() + static_cast<std::ptrdiff_t>(hmacAt),
             note.begin() + static_cast<std::ptrdiff_t>(hmacAt + wire::kHmacTagSize));
  return note;
}

/// What a FLEET_DELIVERY wraps for a long-spatial notification.
std::vector<std::uint8_t> unsignedNote(wire::MessageType type, Bytes payload, std::uint8_t seq,
                                       std::int64_t appId = kAppId) {
  return withoutOwnHmac(notification(type, payload, seq, fleetKey(), appId));
}

/// A plain MESSAGE_BUNDLE (2) over `members`: no HMAC of its own.
std::vector<std::uint8_t> plainBundle(const std::vector<std::vector<std::uint8_t>>& members) {
  std::vector<std::uint8_t> dg = {2};
  for (const auto& m : members) {
    dg.push_back(static_cast<std::uint8_t>(m.size() & 0xff));
    dg.push_back(static_cast<std::uint8_t>(m.size() >> 8));
    dg.insert(dg.end(), m.begin(), m.end());
  }
  return dg;
}

/// A channel notification (18) or channel audio (36): no HMAC in its layout.
std::vector<std::uint8_t> channelNote(std::uint8_t type, std::int64_t channelId, Bytes payload) {
  std::vector<std::uint8_t> f(wire::channel::kHeaderSize + payload.size() +
                              wire::channel::kNotificationTailSize);
  f[0] = type;
  le::writeI64(f.data() + wire::channel::kChannelIdOffset, channelId);
  std::memset(f.data() + wire::channel::kUuidOffset, 'c', 32);
  le::writeU16(f.data() + wire::channel::kPayloadLenOffset, static_cast<std::uint16_t>(payload.size()));
  if (!payload.empty()) std::memcpy(f.data() + wire::channel::kPayloadOffset, payload.data(), payload.size());
  le::writeI64(f.data() + wire::channel::kPayloadOffset + payload.size(), 1700000000001LL);
  f[f.size() - 1] = 9;
  return f;
}

/// MESSAGE_BUNDLE_SIGNED (30) over `members`, signed with the fleet token.
std::vector<std::uint8_t> signedBundle(const std::vector<std::vector<std::uint8_t>>& members) {
  std::vector<std::uint8_t> dg = {30};
  for (const auto& m : members) {
    dg.push_back(static_cast<std::uint8_t>(m.size() & 0xff));
    dg.push_back(static_cast<std::uint8_t>(m.size() >> 8));
    dg.insert(dg.end(), m.begin(), m.end());
  }
  const std::size_t body = dg.size();
  dg.resize(body + wire::kHmacTagSize);
  CHECK(wire::spatialHmac(crypto(), Bytes(dg.data(), body), fleetKey(), dg.data() + body));
  return dg;
}

template <class Done>
void pumpUntil(FleetSession& fleet, Done done) {
  for (int i = 0; i < 100 && !done(); ++i) {
    fleet.pump(20);
    fleet.poll();
  }
}

/// A few passes with nothing expected, so anything wrongly queued would be dispatched. What
/// a fake server sent has arrived already (FakeServer::send waits for it), so the passes
/// wait for nothing.
void settle(FleetSession& fleet) {
  for (int i = 0; i < 3; ++i) {
    fleet.pump(0);
    fleet.poll();
  }
}

/// The session's CLIENT_CAPABILITIES: under its own id, signed with the fleet token. Returns
/// the flags.
std::uint32_t expectCapabilities(FakeServer& server, std::int64_t fleetId = kFleetId,
                                 const std::string& fleetToken = kFleetToken) {
  auto got = server.recvOne();
  CHECK_EQ(got[0], 29u);
  auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
  CHECK(v.ok());
  CHECK_EQ(v->epochMillisOrTokenId, fleetId);
  CHECK(wire::verifyLongSpatial(crypto(), Bytes(got.data(), got.size()), *wire::Token64::fromString(fleetToken))
            .ok());
  CHECK_EQ(v->payload.size(), 4u + wire::kControlNonceSize);
  return le::readU32(v->payload.data());
}

/// Open the session and read the first thing it sends: its capabilities, which bind it.
void openConnected(FleetSession& fleet, FakeServer& server) {
  server.serves(fleet);
  CHECK(fleet.open().ok());
  fleet.pump(0);
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
  CHECK_EQ(expectCapabilities(server), 0x03u);
}

/// `got` is the golden vector but for its last byte, the sequence number, which is `seq`.
void checkGolden(const std::vector<std::uint8_t>& got, const char* goldenHex, std::uint8_t seq) {
  const std::string hex = toHex(got);
  const std::string golden(goldenHex);
  CHECK_EQ(hex.size(), golden.size());
  CHECK_EQ(hex.substr(0, hex.size() - 2), golden.substr(0, golden.size() - 2));
  CHECK_EQ(got.back(), seq);
}

/// The player id a FLEET_CARRY names, after checking the message is the session's own:
/// the capabilities' frame, the fleet id in the tail, signed with the fleet token, and a
/// proof that is HMAC(player token, "ck-fleet-carry" + fleet id + player id + time).
struct Carry {
  std::int64_t player = 0;
  std::int64_t epochMs = 0;
  std::vector<std::uint8_t> proof;
};
Carry readCarry(const std::vector<std::uint8_t>& got, const std::string& playerToken,
                std::int64_t fleetId = kFleetId, const std::string& fleetToken = kFleetToken) {
  CHECK_EQ(got[0], 49u);
  auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
  CHECK(v.ok());
  CHECK_EQ(v->appId, kAppId);
  CHECK(v->chunk == wire::ChunkCoord{});
  CHECK_EQ(v->distance, 0u);
  for (std::size_t i = 0; i < 32; ++i) CHECK_EQ(v->uuid[i], 0);
  CHECK_EQ(v->epochMillisOrTokenId, fleetId);
  CHECK(wire::verifyLongSpatial(crypto(), Bytes(got.data(), got.size()),
                                *wire::Token64::fromString(fleetToken))
            .ok());
  CHECK_EQ(v->payload.size(), 48u);
  Carry c;
  c.player = le::readI64(v->payload.data());
  c.epochMs = le::readI64(v->payload.data() + 8);
  c.proof.assign(v->payload.begin() + 16, v->payload.end());

  std::vector<std::uint8_t> message = {'c', 'k', '-', 'f', 'l', 'e', 'e', 't', '-', 'c', 'a', 'r', 'r', 'y'};
  message.resize(14 + 24);
  le::writeI64(message.data() + 14, fleetId);
  le::writeI64(message.data() + 22, c.player);
  le::writeI64(message.data() + 30, c.epochMs);
  std::uint8_t expected[32];
  CHECK(crypto().hmacSha256(asBytes(playerToken), Bytes(message.data(), message.size()), expected));
  CHECK(std::memcmp(expected, c.proof.data(), 32) == 0);
  return c;
}

/// The tail's game token id of a long-spatial or channel request, and that the fleet token
/// signed it and the player's token did not.
std::int64_t tailIdSignedByFleet(const std::vector<std::uint8_t>& got) {
  const Bytes all(got.data(), got.size());
  const std::size_t signedLen = got.size() - wire::kTailWithHmac;
  std::uint8_t expected[32];
  CHECK(wire::spatialHmac(crypto(), all.first(signedLen), fleetKey(), expected));
  CHECK(std::memcmp(expected, got.data() + signedLen, 32) == 0);
  std::uint8_t players[32];
  CHECK(wire::spatialHmac(crypto(), all.first(signedLen), playerKey(), players));
  CHECK(std::memcmp(players, got.data() + signedLen, 32) != 0);
  return le::readI64(got.data() + got.size() - wire::kTailNoHmac);
}

struct Seen {
  std::vector<FleetCarryResult> results;
  std::vector<FleetReleased> released;
  std::vector<ConnState> states;
  std::vector<GenericError> errors;
};

/// Handlers that record the carry results, the releases and the states.
FleetHandlers recording(Seen& seen) {
  FleetHandlers h;
  h.carryResult = [&seen](const FleetCarryResult& r) { seen.results.push_back(r); };
  h.released = [&seen](const FleetReleased& r) { seen.released.push_back(r); };
  h.status = [&seen](ConnState s) { seen.states.push_back(s); };
  h.genericError = [&seen](const GenericError& e) { seen.errors.push_back(e); };
  return h;
}

/// Carry `id` and answer it carried as `user`; returns once the session says so.
void carryAs(FleetSession& fleet, FakeServer& server, std::int64_t id, std::int64_t user,
             const std::string& token) {
  CHECK(fleet.carry({token, id}).ok());
  auto got = server.recvOne();
  CHECK_EQ(readCarry(got, token).player, id);
  server.send(own(carryResultFrame(id, user, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(id); });
  CHECK(fleet.carries(id));
}

SpatialSend goldenUpdate(const std::uint8_t (&pose)[4]) {
  SpatialSend send;
  send.chunk = {1, -2, 3};
  send.uuid = uuidOf("0123456789abcdef0123456789abcdef");
  send.payload = Bytes(pose, sizeof(pose));
  return send;
}

// The server's frames with their own HMAC, as wire_test holds them (computed there by a
// short Python script, independently of the code): FLEET_CARRY_RESULT for player 4242 as
// user 77, FLEET_RELEASED for player 4242 reason 1, FLEET_DELIVERY for players 4242 and 4343
// around spatial text "hi", and FLEET_DELIVERY for player 4242 around channel 55's text "hi".
const char kGoldenSignedCarryResult[] =
    "3392100000000000004d00000000000000001c1bb3a5ad36f54e3798c7818a8a43fd87537625d7a7ebcfabe6"
    "c18ff7c7f51e";
const char kGoldenSignedReleased[] =
    "34921000000000000001bbe798feb8e0fd590bbdc5bcedde2defcd4cdbb17f759615c134a068cf7c312b";
const char kGoldenSignedDeliverySpatial[] =
    "35029210000000000000f7100000000000008907000000000000000100000000000000feffffffffffffff03"
    "0000000000000008000030313233343536373839616263646566303132333435363738396162636465666869"
    "005e56eea101000005edb21ec31a3ccf46be239853625267e4c8ce43b053687b83f4e6e4344fa37146";
const char kGoldenSignedDeliveryChannel[] =
    "3501921000000000000012370000000000000030313233343536373839616263646566303132333435363738"
    "3961626364656602006869005e56eea101000006ac80c74ade9f61f57cac9d4b3fa729d908f9a1e049222a44"
    "e854a2359cf2ed53";

std::vector<std::uint8_t> fromHex(const char* hex) {
  std::vector<std::uint8_t> out;
  auto nibble = [](char c) { return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
  for (const char* c = hex; c[0] && c[1]; c += 2)
    out.push_back(static_cast<std::uint8_t>((nibble(c[0]) << 4) | nibble(c[1])));
  return out;
}

/// The next datagram of `type`, skipping any other (a capabilities advertisement, say).
std::vector<std::uint8_t> nextOfType(FakeServer& server, std::uint8_t type) {
  for (int i = 0; i < 64; ++i) {
    auto got = server.recvOne();
    if (got[0] == type) return got;
  }
  CHECK(false);
  return {};
}

/// Pump the session until the server has a datagram of FLEET_CARRY's type, and return it.
std::vector<std::uint8_t> nextOfTypeAfterPump(FleetSession& fleet, FakeServer& server) {
  for (int i = 0; i < 100; ++i) {
    fleet.pump(5);
    fleet.poll();
    auto got = server.tryRecv(5);
    if (!got.empty() && got[0] == 49) return got;
  }
  CHECK(false);
  return {};
}

/// Everything the server has been sent and not read yet.
std::vector<std::vector<std::uint8_t>> drain(FakeServer& server) { return server.takeAll(); }

/// The tail's game token id of a request (long-spatial and channel requests end alike).
std::int64_t tailId(const std::vector<std::uint8_t>& datagram) {
  return le::readI64(datagram.data() + datagram.size() - wire::kTailNoHmac);
}

/// One notification as a handler saw it.
struct Note {
  std::uint8_t type = 0;
  bool forSession = false;
  std::vector<std::int64_t> players;
  std::vector<std::uint8_t> payload;
  std::int64_t detail = 0;  // a channel's id, a voxel's type, an event's type
  std::uint8_t sequence = 0;
};

auto spatialInto(std::vector<Note>& into) {
  return [&into](const SpatialNotification& n, FleetRecipients r) {
    into.push_back({static_cast<std::uint8_t>(n.type), r.forSession(), {r.begin(), r.end()},
                    {n.payload.begin(), n.payload.end()}, 0, n.sequence});
  };
}

auto channelInto(std::vector<Note>& into, std::uint8_t type) {
  return [&into, type](const ChannelNotification& n, FleetRecipients r) {
    into.push_back({type, r.forSession(), {r.begin(), r.end()}, {n.payload.begin(), n.payload.end()},
                    n.channelId, n.sequence});
  };
}

// ---------------------------------------------------------------------------
// Carry, send for the player, release: the golden bytes, both ways.
// ---------------------------------------------------------------------------

void runCarrySendRelease() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  std::vector<Note> texts, channelTexts;
  FleetHandlers handlers = recording(seen);
  handlers.text = spatialInto(texts);
  handlers.channelMessage = channelInto(channelTexts, 18);
  fleet.setHandlers(std::move(handlers));

  const std::uint8_t pose[] = {0xde, 0xad, 0xbe, 0xef};
  const SpatialSend update = goldenUpdate(pose);

  // Nothing before open(); a malformed token and the session's own id are refused.
  CHECK_EQ(static_cast<int>(fleet.carry({kPlayerToken, kPlayerId}).code),
           static_cast<int>(Errc::NotConnected));
  openConnected(fleet, server);  // its capabilities are the first thing on the wire
  CHECK_EQ(static_cast<int>(fleet.carry({"short", kPlayerId}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK_EQ(static_cast<int>(fleet.carry({kPlayerToken, kFleetId}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK(fleet.players().empty());

  // A player the session does not carry: refused here, nothing on the wire.
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(kPlayerId, update).error()),
           static_cast<int>(Errc::NotCarried));
  CHECK_EQ(std::string(errcName(Errc::NotCarried)), std::string("NotCarried"));
  CHECK_EQ(fleet.stats().messagesSent, 1u);  // the capabilities
  server.expectNothing();

  // FLEET_CARRY: the golden bytes, and a proof an HMAC computed here agrees with.
  std::vector<FleetCarryResult> viaCallback;
  CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                viaCallback.push_back(r);
              }).ok());
  auto carry = server.recvOne();
  checkGolden(carry, kGoldenFleetCarry, 1);
  const Carry read = readCarry(carry, kPlayerToken);
  CHECK_EQ(read.player, kPlayerId);
  CHECK_EQ(read.epochMs, kEpochMs);
  // Signed with the fleet token and not with the player's.
  CHECK_EQ(static_cast<int>(
               wire::verifyLongSpatial(crypto(), Bytes(carry.data(), carry.size()), playerKey()).code),
           static_cast<int>(Errc::HmacMismatch));
  CHECK_EQ(fleet.stats().messagesSentByType[49], 1u);

  // Until the result arrives the player is not carried.
  auto row = fleet.player(kPlayerId);
  CHECK(row.has_value());
  CHECK(row->pending && !row->carried);
  CHECK_EQ(row->userId, 0);
  CHECK(!fleet.carries(kPlayerId));
  CHECK_EQ(fleet.carriedCount(), 0u);
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(kPlayerId, update).error()),
           static_cast<int>(Errc::NotCarried));
  settle(fleet);
  CHECK(viaCallback.empty());
  server.expectNothing();

  // FLEET_CARRY_RESULT, the golden frame with its own HMAC: once to the callback, then once
  // to the handler.
  server.send(fromHex(kGoldenSignedCarryResult));
  pumpUntil(fleet, [&] { return !seen.results.empty(); });
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK_EQ(seen.results.size(), 1u);
  CHECK(viaCallback[0].carried());
  CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::Carried));
  CHECK_EQ(viaCallback[0].playerGameTokenId, kPlayerId);
  CHECK_EQ(viaCallback[0].userId, 77);
  CHECK_EQ(viaCallback[0].wireStatus, 0u);
  CHECK_EQ(seen.results[0].userId, 77);
  row = fleet.player(kPlayerId);
  CHECK(row.has_value());
  CHECK(row->carried && !row->pending);
  CHECK_EQ(row->userId, 77);
  CHECK_EQ(fleet.carriedCount(), 1u);
  CHECK_EQ(fleet.players().size(), 1u);
  CHECK_EQ(fleet.stats().messagesReceivedByType[51], 1u);

  // A second answer to the same carry (the carry was sent twice) reports nothing.
  server.send(fromHex(kGoldenSignedCarryResult));
  server.send(own(carryResultFrame(kPlayerId, 0, 1)));
  settle(fleet);
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK_EQ(seen.results.size(), 1u);
  CHECK(fleet.carries(kPlayerId));

  // The player's actor update: the golden bytes. The tail carries the PLAYER's id, and
  // the HMAC verifies with the FLEET token and not with the player's.
  auto seq = fleet.sendActorUpdate(kPlayerId, update);
  CHECK(seq.ok());
  CHECK_EQ(seq.value(), 2u);
  auto actor = server.recvOne();
  checkGolden(actor, kGoldenCarriedActorUpdate, 2);
  auto parsed = wire::parseLongSpatial(Bytes(actor.data(), actor.size()));
  CHECK(parsed.ok());
  CHECK_EQ(parsed->epochMillisOrTokenId, kPlayerId);
  CHECK(wire::verifyLongSpatial(crypto(), Bytes(actor.data(), actor.size()), fleetKey()).ok());
  CHECK_EQ(static_cast<int>(
               wire::verifyLongSpatial(crypto(), Bytes(actor.data(), actor.size()), playerKey()).code),
           static_cast<int>(Errc::HmacMismatch));

  // Another player's id is still refused, by every send, with nothing sent.
  const std::int64_t stranger = 5555;
  const std::uint64_t sentBefore = fleet.stats().messagesSent;
  const Bytes body(pose, sizeof(pose));
  const auto refused = [](const auto& r) { return r.error() == Errc::NotCarried; };
  CHECK(refused(fleet.sendActorUpdate(stranger, update)));
  CHECK(refused(fleet.sendVoxelUpdate(stranger, {1, 2, 3}, uuid('a'), 1, 2, 3, 4, body)));
  CHECK(refused(fleet.sendAudio(stranger, update)));
  CHECK(refused(fleet.sendVideo(stranger, update)));
  CHECK(refused(fleet.sendVideoFrame(stranger, {1, 2, 3}, uuid('a'), body, 1)));
  CHECK(refused(fleet.sendText(stranger, update)));
  CHECK(refused(fleet.sendClientEvent(stranger, {1, 2, 3}, uuid('a'), 5, body)));
  CHECK(refused(fleet.sendGenericSpatial(stranger, update)));
  CHECK(refused(fleet.sendSingleActorMessage(stranger, {1, 2, 3}, uuid('a'), body)));
  CHECK(refused(fleet.sendChannelMessage(stranger, 55, uuid('a'), body)));
  CHECK(refused(fleet.sendChannelAudio(stranger, 55, uuid('a'), body)));
  CHECK(refused(fleet.sendRangedChannelMessage(stranger, 55, uuid('a'), body, {0, 0, 0}, 4)));
  CHECK(refused(fleet.sendHeartbeat(stranger, {1, 2, 3}, uuid('a'))));
  CHECK_EQ(static_cast<int>(fleet.setSpeakers(stranger, SpeakerList::everyone(), SpeakerList::keep()).code),
           static_cast<int>(Errc::NotCarried));
  CHECK_EQ(fleet.stats().messagesSent, sentBefore);
  server.expectNothing();

  // FLEET_DELIVERY, the golden frames: spatial text for players 4242 and 4343, then channel
  // 55's text for player 4242. The notification inside is the unsigned form.
  server.send(fromHex(kGoldenSignedDeliverySpatial));
  pumpUntil(fleet, [&] { return !texts.empty(); });
  CHECK_EQ(texts.size(), 1u);
  CHECK(!texts[0].forSession);
  CHECK(texts[0].players == std::vector<std::int64_t>({4242, 4343}));
  CHECK(texts[0].payload == std::vector<std::uint8_t>({'h', 'i'}));
  CHECK_EQ(texts[0].type, 137u);
  CHECK_EQ(texts[0].sequence, 5u);
  server.send(fromHex(kGoldenSignedDeliveryChannel));
  pumpUntil(fleet, [&] { return !channelTexts.empty(); });
  CHECK_EQ(channelTexts.size(), 1u);
  CHECK(!channelTexts[0].forSession);
  CHECK(channelTexts[0].players == std::vector<std::int64_t>({4242}));
  CHECK(channelTexts[0].payload == std::vector<std::uint8_t>({'h', 'i'}));
  CHECK_EQ(channelTexts[0].detail, 55);
  CHECK_EQ(channelTexts[0].sequence, 6u);
  CHECK_EQ(fleet.stats().messagesReceivedByType[53], 2u);
  CHECK_EQ(fleet.stats().hmacFailures, 0u);
  CHECK_EQ(fleet.stats().malformed, 0u);

  // FLEET_RELEASE: the golden bytes, and the player is out of the table at once.
  CHECK(fleet.release(kPlayerId).ok());
  auto release = server.recvOne();
  checkGolden(release, kGoldenFleetRelease, 3);
  auto releaseParsed = wire::parseLongSpatial(Bytes(release.data(), release.size()));
  CHECK(releaseParsed.ok());
  CHECK_EQ(releaseParsed->epochMillisOrTokenId, kFleetId);
  CHECK_EQ(releaseParsed->payload.size(), 8u);
  CHECK_EQ(le::readI64(releaseParsed->payload.data()), kPlayerId);
  CHECK(wire::verifyLongSpatial(crypto(), Bytes(release.data(), release.size()), fleetKey()).ok());
  CHECK(!fleet.carries(kPlayerId));
  CHECK(!fleet.player(kPlayerId).has_value());
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(kPlayerId, update).error()),
           static_cast<int>(Errc::NotCarried));
  CHECK_EQ(fleet.stats().messagesSentByType[50], 1u);

  // Carried again, then FLEET_RELEASED, the golden frame: another session carries the player.
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);
  server.send(fromHex(kGoldenSignedReleased));
  pumpUntil(fleet, [&] { return !seen.released.empty(); });
  CHECK_EQ(seen.released.size(), 1u);
  CHECK_EQ(seen.released[0].playerGameTokenId, kPlayerId);
  CHECK_EQ(seen.released[0].userId, 77);
  CHECK_EQ(static_cast<int>(seen.released[0].reason), static_cast<int>(FleetReleaseReason::CarriedElsewhere));
  CHECK(!fleet.carries(kPlayerId));

  // close() empties the table and ends carrying.
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);
  fleet.close();
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Closed));
  CHECK(fleet.players().empty());
  CHECK_EQ(static_cast<int>(fleet.carry({kPlayerToken, kPlayerId}).code),
           static_cast<int>(Errc::NotConnected));
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(kPlayerId, update).error()),
           static_cast<int>(Errc::NotCarried));
  CHECK_EQ(static_cast<int>(fleet.release(kPlayerId).code), static_cast<int>(Errc::NotConnected));
  CHECK_EQ(static_cast<int>(fleet.sendHeartbeat().error()), static_cast<int>(Errc::NotConnected));
}

// ---------------------------------------------------------------------------
// Every send Connection offers, for a carried player: the player's id in the tail, the
// fleet token's HMAC.
// ---------------------------------------------------------------------------

void runEverySendForAPlayer() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  openConnected(fleet, server);
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);

  const std::uint8_t body[] = {1, 2, 3, 4};
  const Bytes bytes(body, sizeof(body));
  SpatialSend p;
  p.chunk = {4, 5, 6};
  p.uuid = uuid('a');
  p.payload = bytes;

  auto expect = [&](std::uint8_t type) {
    auto got = server.recvOne();
    CHECK_EQ(got[0], type);
    CHECK_EQ(tailIdSignedByFleet(got), kPlayerId);
    return got;
  };

  CHECK(fleet.sendActorUpdate(kPlayerId, p).ok());
  expect(128);
  CHECK(fleet.sendVoxelUpdate(kPlayerId, {4, 5, 6}, uuid('a'), 1, 2, 3, 9, bytes).ok());
  {
    auto got = expect(131);
    auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
    CHECK(v.ok());
    auto voxel = wire::parseVoxelPayload(v->payload);
    CHECK(voxel.ok());
    CHECK_EQ(voxel->voxelType, 9);
    CHECK_EQ(voxel->state.size(), 4u);
  }
  CHECK(fleet.sendAudio(kPlayerId, p).ok());
  CHECK_EQ(expect(134)[wire::offsets::kDistance], 1u);  // voice defaults to 1 chunk
  CHECK(fleet.sendVideo(kPlayerId, p).ok());
  CHECK_EQ(expect(143)[wire::offsets::kDistance], 1u);
  auto fragments = fleet.sendVideoFrame(kPlayerId, {4, 5, 6}, uuid('a'), bytes, 3);
  CHECK(fragments.ok());
  CHECK_EQ(fragments.value(), 1u);
  expect(143);
  CHECK(fleet.sendText(kPlayerId, p).ok());
  CHECK_EQ(expect(136)[wire::offsets::kDistance], 8u);
  CHECK(fleet.sendClientEvent(kPlayerId, {4, 5, 6}, uuid('a'), 0x0102, bytes).ok());
  {
    auto got = expect(138);
    auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
    CHECK(v.ok());
    auto ev = wire::parseEventPayload(v->payload);
    CHECK(ev.ok());
    CHECK_EQ(ev->eventType, 0x0102u);
  }
  CHECK(fleet.sendGenericSpatial(kPlayerId, p).ok());
  expect(140);
  CHECK(fleet.sendSingleActorMessage(kPlayerId, {4, 5, 6}, uuid('z'), bytes).ok());
  expect(142);
  CHECK(fleet.sendChannelMessage(kPlayerId, 55, uuid('a'), bytes).ok());
  {
    auto got = expect(17);
    CHECK_EQ(le::readI64(got.data() + wire::channel::kChannelIdOffset), 55);
    CHECK_EQ(got.size(), wire::channelRequestSize(4));
  }
  CHECK(fleet.sendChannelAudio(kPlayerId, 55, uuid('a'), bytes).ok());
  expect(35);
  CHECK(fleet.sendRangedChannelMessage(kPlayerId, 55, uuid('a'), bytes, {3, 4, -5}, 12).ok());
  {
    auto got = expect(32);
    CHECK_EQ(le::readI64(got.data() + wire::channel_ranged::kAppIdOffset), kAppId);
    CHECK_EQ(le::readU32(got.data() + wire::channel_ranged::kMaxDistanceOffset), 12u);
  }
  // The keep-alive of the player's actor names the player who owns it.
  CHECK(fleet.sendHeartbeat(kPlayerId, {4, 5, 6}, uuid('a')).ok());
  {
    auto got = expect(26);
    CHECK(std::memcmp(got.data() + wire::offsets::kUuid, uuid('a').data(), 32) == 0);
  }
  // The player's speaker lists: one kind a message, under the PLAYER's id.
  CHECK(fleet.setSpeakers(kPlayerId, SpeakerList::only({uuid('s'), uuid('t')}), SpeakerList::only({})).ok());
  {
    auto voice = expect(47);
    auto v = wire::parseLongSpatial(Bytes(voice.data(), voice.size()));
    CHECK(v.ok());
    CHECK_EQ(v->payload.size(), 4u + 64u);
    CHECK_EQ(v->payload[0], 1u);
    CHECK_EQ(v->payload[1], 2u);
    CHECK_EQ(v->payload[2], 0u);
    auto video = expect(47);
    auto w = wire::parseLongSpatial(Bytes(video.data(), video.size()));
    CHECK(w.ok());
    CHECK_EQ(w->payload.size(), 4u);
    CHECK_EQ(w->payload[2], 1u);
    CHECK_EQ(w->payload[3], 0u);
  }
  std::vector<core::ActorUuid> tooMany(wire::kMaxSpeakers + 1, uuid('s'));
  CHECK_EQ(static_cast<int>(fleet.setSpeakers(kPlayerId, SpeakerList::only(tooMany), SpeakerList::keep()).code),
           static_cast<int>(Errc::InvalidArgument));

  // The session's own keepalive and capabilities: no actor, chunk (0,0,0), the FLEET id.
  CHECK(fleet.sendHeartbeat().ok());
  {
    auto got = server.recvOne();
    CHECK_EQ(got[0], 26u);
    auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
    CHECK(v.ok());
    CHECK(v->chunk == wire::ChunkCoord{});
    CHECK_EQ(v->distance, 0u);
    CHECK_EQ(v->payload.size(), 0u);
    for (std::size_t i = 0; i < 32; ++i) CHECK_EQ(v->uuid[i], 0);
    CHECK_EQ(v->epochMillisOrTokenId, kFleetId);
    CHECK(wire::verifyLongSpatial(crypto(), Bytes(got.data(), got.size()), fleetKey()).ok());
  }
  CHECK(fleet.sendCapabilities().ok());
  CHECK_EQ(expectCapabilities(server), 0x03u);
}

// ---------------------------------------------------------------------------
// Under the session's own id the server takes 29, 49, 50 and 26 and nothing else, and it
// refuses 29 under a player's id. Everything a session can put on the wire is checked.
// ---------------------------------------------------------------------------

void runOnlyFourMessagesUnderTheSessionsId() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  cfg.transport.advertiseIntervalMs = 15'000;
  cfg.transport.advertiseCapabilities = false;  // not an option for a fleet session
  FleetSession fleet(cfg, crypto(), clock);
  server.serves(fleet);
  CHECK(fleet.open().ok());
  fleet.pump(0);

  std::vector<std::vector<std::uint8_t>> all;
  const auto collect = [&] {
    for (auto& d : drain(server)) all.push_back(std::move(d));
  };
  collect();
  CHECK_EQ(all.size(), 1u);  // the capabilities leave whatever the transport option says
  CHECK_EQ(all[0][0], 29u);

  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  collect();
  server.send(own(carryResultFrame(kPlayerId, 77, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  CHECK(fleet.carries(kPlayerId));

  const std::uint8_t body[] = {1, 2, 3, 4};
  const Bytes bytes(body, sizeof(body));
  SpatialSend p;
  p.chunk = {4, 5, 6};
  p.uuid = uuid('a');
  p.payload = bytes;
  // Every send for a player.
  CHECK(fleet.sendActorUpdate(kPlayerId, p).ok());
  CHECK(fleet.sendVoxelUpdate(kPlayerId, {4, 5, 6}, uuid('a'), 1, 2, 3, 9, bytes).ok());
  CHECK(fleet.sendAudio(kPlayerId, p).ok());
  CHECK(fleet.sendVideo(kPlayerId, p).ok());
  CHECK(fleet.sendVideoFrame(kPlayerId, {4, 5, 6}, uuid('a'), bytes, 3).ok());
  CHECK(fleet.sendText(kPlayerId, p).ok());
  CHECK(fleet.sendClientEvent(kPlayerId, {4, 5, 6}, uuid('a'), 7, bytes).ok());
  CHECK(fleet.sendGenericSpatial(kPlayerId, p).ok());
  CHECK(fleet.sendSingleActorMessage(kPlayerId, {4, 5, 6}, uuid('z'), bytes).ok());
  CHECK(fleet.sendChannelMessage(kPlayerId, 55, uuid('a'), bytes).ok());
  CHECK(fleet.sendChannelAudio(kPlayerId, 55, uuid('a'), bytes).ok());
  CHECK(fleet.sendRangedChannelMessage(kPlayerId, 55, uuid('a'), bytes, {3, 4, -5}, 12).ok());
  CHECK(fleet.sendHeartbeat(kPlayerId, {4, 5, 6}, uuid('a')).ok());
  CHECK(fleet.setSpeakers(kPlayerId, SpeakerList::only({uuid('s')}), SpeakerList::only({uuid('t')})).ok());
  // Everything of the session's own, and everything the library sends by itself.
  CHECK(fleet.sendHeartbeat().ok());
  CHECK(fleet.sendCapabilities().ok());
  fleet.setFleetOwnState(true);
  fleet.setOwnMediaEcho(true);
  clock.advance(15'000);
  fleet.pump(0);  // the periodic advertisement, and the player's speaker lists after it
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());  // a carry again with the same token
  CHECK(fleet.release(kPlayerId).ok());
  collect();

  int ownTypes[256] = {};
  int playerTypes[256] = {};
  for (const auto& d : all) {
    const std::uint8_t type = d[0];
    const std::int64_t id = tailId(d);
    if (id == kFleetId) {
      CHECK(type == 29 || type == 49 || type == 50 || type == 26);
      ++ownTypes[type];
    } else {
      CHECK_EQ(id, kPlayerId);
      CHECK(type != 29 && type != 49 && type != 50);
      ++playerTypes[type];
    }
  }
  // Each of the four was sent under the session's id, and a speaker list never was.
  CHECK(ownTypes[29] >= 5);  // connect, sendCapabilities, two setters, the periodic one
  CHECK_EQ(ownTypes[49], 2);
  CHECK_EQ(ownTypes[50], 1);
  CHECK_EQ(ownTypes[26], 1);
  CHECK_EQ(ownTypes[47], 0);
  CHECK_EQ(playerTypes[47], 4);  // two kinds when set, and again after the advertisement
  CHECK_EQ(playerTypes[26], 1);
  CHECK_EQ(playerTypes[128], 1);
  CHECK_EQ(playerTypes[17], 1);
}

// ---------------------------------------------------------------------------
// A carry can go unanswered: it is sent again every 250 ms with a NEW time and proof, and
// ends TimedOut 5 s after its first send.
// ---------------------------------------------------------------------------

void runCarryResendAndDeadline() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  CHECK_EQ(cfg.carryResendMs, 250);
  CHECK_EQ(cfg.carryTimeoutMs, 5000);
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);

  std::vector<FleetCarryResult> viaCallback;
  CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                viaCallback.push_back(r);
              }).ok());
  const Carry first = readCarry(server.recvOne(), kPlayerToken);
  CHECK_EQ(first.epochMs, kEpochMs);

  // Not before its interval.
  clock.advance(249);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();
  CHECK_EQ(fleet.stats().messagesSentByType[49], 1u);

  // At 250 ms: again, with the time of the resend and the proof for that time. The same
  // bytes again would be a replay once the first was accepted and ended.
  clock.advance(1);
  fleet.pump(0);
  const Carry second = readCarry(server.recvOne(), kPlayerToken);
  CHECK_EQ(second.player, kPlayerId);
  CHECK_EQ(second.epochMs, kEpochMs + 250);
  CHECK(second.proof != first.proof);
  // One pass sends it once.
  fleet.pump(0);
  server.expectNothing();

  // Every 250 ms up to 4,750 ms: 20 carries in all, each with its own time and proof, and
  // no result reported yet.
  std::vector<std::vector<std::uint8_t>> proofs = {first.proof, second.proof};
  for (int step = 2; step <= 19; ++step) {
    clock.advance(250);
    fleet.pump(0);
    fleet.poll();
    const Carry again = readCarry(server.recvOne(), kPlayerToken);
    CHECK_EQ(again.epochMs, kEpochMs + 250 * step);
    for (const auto& earlier : proofs) CHECK(again.proof != earlier);
    proofs.push_back(again.proof);
  }
  CHECK_EQ(fleet.stats().messagesSentByType[49], 20u);
  CHECK(viaCallback.empty());
  CHECK(seen.results.empty());
  CHECK(fleet.player(kPlayerId).has_value());

  // At 5,000 ms the carry ends: nothing more is sent, and poll() reports it once.
  clock.advance(250);
  fleet.pump(0);
  server.expectNothing();
  CHECK(viaCallback.empty());  // reported from poll(), like every callback
  fleet.poll();
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK_EQ(seen.results.size(), 1u);
  CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::TimedOut));
  CHECK(!viaCallback[0].carried());
  CHECK_EQ(viaCallback[0].playerGameTokenId, kPlayerId);
  CHECK_EQ(viaCallback[0].userId, 0);
  CHECK_EQ(viaCallback[0].wireStatus, 255u);
  CHECK_EQ(std::string(fleetCarryStatusName(viaCallback[0].status)), std::string("TimedOut"));
  CHECK(!fleet.player(kPlayerId).has_value());
  CHECK_EQ(fleet.stats().messagesSentByType[49], 20u);

  // An answer that arrives after the deadline is for nobody.
  server.send(own(carryResultFrame(kPlayerId, 77, 0)));
  settle(fleet);
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK(!fleet.carries(kPlayerId));
  clock.advance(1000);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();

  // A result stops the resends of a later carry.
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  readCarry(server.recvOne(), kPlayerToken);
  server.send(own(carryResultFrame(kPlayerId, 77, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  clock.advance(1000);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();
  CHECK(fleet.carries(kPlayerId));
  CHECK_EQ(seen.results.size(), 2u);
  CHECK(seen.results[1].carried());

  // A carry made before the session is connected leaves once it is, after its capabilities.
  FakeServer later;
  later.start();
  FleetSession waiting(configFor(later), crypto(), clock);
  later.serves(waiting);
  CHECK(waiting.open().ok());
  CHECK_EQ(static_cast<int>(waiting.state()), static_cast<int>(ConnState::Connecting));
  CHECK(waiting.carry({kPlayerToken, kPlayerId}).ok());
  CHECK_EQ(waiting.stats().messagesSent, 0u);
  waiting.pump(0);
  CHECK_EQ(expectCapabilities(later), 0x03u);
  CHECK_EQ(readCarry(later.recvOne(), kPlayerToken).player, kPlayerId);
}

// ---------------------------------------------------------------------------
// The session sends at most FleetConfig::carriesPerSecond carries a second after a burst
// of carryBurst (the server drops carries past 64 a second unanswered), and a carry that
// never left goes before one that is being sent again.
// ---------------------------------------------------------------------------

void runCarriesArePaced() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  CHECK_EQ(cfg.carriesPerSecond, 50);
  CHECK_EQ(cfg.carryBurst, 32);
  CHECK(cfg.carriesPerSecond < 64);
  FleetSession fleet(cfg, crypto(), clock);
  openConnected(fleet, server);

  const int players = 80;
  for (int i = 0; i < players; ++i) CHECK(fleet.carry({tokenFor(1000 + i), 1000 + i}).ok());

  std::vector<int> sends(players, 0);
  int total = 0;
  const auto read = [&] {
    for (const auto& d : drain(server)) {
      CHECK_EQ(d[0], 49u);
      const std::int64_t id = le::readI64(d.data() + wire::offsets::kPayload);
      CHECK(id >= 1000 && id < 1000 + players);
      readCarry(d, tokenFor(id));
      ++sends[static_cast<std::size_t>(id - 1000)];
      ++total;
    }
  };
  const auto distinct = [&] {
    int n = 0;
    for (int s : sends) n += s > 0 ? 1 : 0;
    return n;
  };

  // The burst at once, and no more however often the session is pumped.
  read();
  CHECK_EQ(total, 32);
  CHECK_EQ(distinct(), 32);
  fleet.pump(0);
  fleet.pump(0);
  read();
  CHECK_EQ(total, 32);
  CHECK_EQ(fleet.stats().messagesSentByType[49], 32u);
  // The carries that wait have not started their timeout.
  for (int i = 0; i < players; ++i) CHECK(fleet.player(1000 + i)->pending);

  // 200 ms: 10 more, each a player that had not been sent.
  clock.advance(200);
  fleet.pump(0);
  read();
  CHECK_EQ(total, 42);
  CHECK_EQ(distinct(), 42);

  // A second in 100 ms steps: never more than the burst and 50 a second, and every player
  // has been sent before any is sent twice... until all have left once.
  for (int step = 0; step < 8; ++step) {
    clock.advance(100);
    fleet.pump(0);
    read();
    const int elapsedMs = 200 + 100 * (step + 1);
    CHECK(total <= 32 + 50 * elapsedMs / 1000);
    if (distinct() < players) {
      for (int s : sends) CHECK(s <= 1);
    }
  }
  CHECK_EQ(distinct(), players);  // 32 + 48 by the first second
  CHECK_EQ(total, 32 + 50);       // the two turns left went to the carries that waited longest
  int twice = 0;
  for (int s : sends) twice += s == 2 ? 1 : 0;
  CHECK_EQ(twice, 2);

  // Left unanswered, the resends keep to the rate: 50 in the next second, and nothing ends
  // TimedOut before 5 s from its own first send.
  int timedOut = 0;
  FleetHandlers h;
  h.carryResult = [&](const FleetCarryResult& r) {
    if (r.status == FleetCarryStatus::TimedOut) ++timedOut;
  };
  fleet.setHandlers(std::move(h));
  const int before = total;
  for (int step = 0; step < 10; ++step) {
    clock.advance(100);
    fleet.pump(0);
    fleet.poll();
    read();
  }
  CHECK_EQ(total - before, 50);
  CHECK_EQ(timedOut, 0);
  // Answers end the resends.
  for (int i = 0; i < players; ++i) server.send(own(carryResultFrame(1000 + i, 5000 + i, 0)));
  pumpUntil(fleet, [&] { return fleet.carriedCount() == static_cast<std::size_t>(players); });
  CHECK_EQ(fleet.carriedCount(), static_cast<std::size_t>(players));
  read();
  const int settled = total;
  clock.advance(1000);
  fleet.pump(0);
  read();
  CHECK_EQ(total, settled);
}

// ---------------------------------------------------------------------------
// Each FLEET_CARRY_RESULT status.
// ---------------------------------------------------------------------------

void runCarryStatuses() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);

  struct Case {
    std::uint8_t wire;
    FleetCarryStatus status;
    const char* name;
  };
  const Case cases[] = {
      {1, FleetCarryStatus::NoLiveToken, "NoLiveToken"},              // also a token of another app
      {2, FleetCarryStatus::OtherOrganization, "OtherOrganization"},  // another organization's player
      {3, FleetCarryStatus::ProofRefused, "ProofRefused"},            // the proof, its time, or a replay
      {4, FleetCarryStatus::NoRoom, "NoRoom"},                        // the session is full, or no place for it
      {9, FleetCarryStatus::Refused, "Refused"},                      // a status this build does not know
  };
  for (const Case& c : cases) {
    std::vector<FleetCarryResult> viaCallback;
    CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                  viaCallback.push_back(r);
                }).ok());
    readCarry(server.recvOne(), kPlayerToken);
    const std::size_t before = seen.results.size();
    // A refusal's user id is 0 on the wire; one that is not must not be believed.
    server.send(own(carryResultFrame(kPlayerId, c.wire == 2 ? 77 : 0, c.wire)));
    if (c.wire == 1) {
      // "No live token" is asked about once more before it settles
      // (runNoLiveTokenIsAskedOnceMore): the second answer is the refusal.
      settle(fleet);
      CHECK(viaCallback.empty());
      clock.advance(250);
      fleet.pump(0);
      readCarry(server.recvOne(), kPlayerToken);
      server.send(own(carryResultFrame(kPlayerId, 0, c.wire)));
    }
    pumpUntil(fleet, [&] { return seen.results.size() > before; });
    CHECK_EQ(viaCallback.size(), 1u);
    CHECK_EQ(seen.results.size(), before + 1);
    CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(c.status));
    CHECK_EQ(viaCallback[0].wireStatus, c.wire);
    CHECK(!viaCallback[0].carried());
    CHECK_EQ(viaCallback[0].userId, 0);
    CHECK_EQ(viaCallback[0].playerGameTokenId, kPlayerId);
    CHECK_EQ(std::string(fleetCarryStatusName(viaCallback[0].status)), std::string(c.name));
    // Refused: not carried, not sent again, and sends for the player are refused here.
    CHECK(!fleet.player(kPlayerId).has_value());
    clock.advance(1000);
    fleet.pump(0);
    fleet.poll();
    server.expectNothing();
    CHECK_EQ(viaCallback.size(), 1u);
    CHECK_EQ(static_cast<int>(fleet.sendHeartbeat(kPlayerId, {0, 0, 0}, uuid('a')).error()),
             static_cast<int>(Errc::NotCarried));
  }
  // The wire's values are the enum's.
  CHECK_EQ(static_cast<int>(FleetCarryStatus::Carried), 0);
  CHECK_EQ(static_cast<int>(FleetCarryStatus::NoLiveToken), 1);
  CHECK_EQ(static_cast<int>(FleetCarryStatus::OtherOrganization), 2);
  CHECK_EQ(static_cast<int>(FleetCarryStatus::ProofRefused), 3);
  CHECK_EQ(static_cast<int>(FleetCarryStatus::NoRoom), 4);

  // Status 0, with the user id.
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  readCarry(server.recvOne(), kPlayerToken);
  server.send(own(carryResultFrame(kPlayerId, 123456789012LL, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  CHECK(seen.results.back().carried());
  CHECK_EQ(seen.results.back().userId, 123456789012LL);
  CHECK_EQ(std::string(fleetCarryStatusName(seen.results.back().status)), std::string("Carried"));

  // A result for a player the session never carried changes nothing. One without its HMAC
  // is no frame at all, and one whose HMAC does not hold is dropped and counted.
  const std::size_t before = seen.results.size();
  server.send(own(carryResultFrame(31337, 5, 0)));
  server.send(carryResultFrame(kPlayerId, 77, 0));  // 18 bytes, alone: nothing vouches for it
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 1; });
  auto cut = own(carryResultFrame(kPlayerId, 77, 0));
  cut.pop_back();
  server.send(cut);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  settle(fleet);
  CHECK_EQ(seen.results.size(), before);
  CHECK(!fleet.player(31337).has_value());
  CHECK_EQ(fleet.stats().malformed, 1u);
  CHECK_EQ(fleet.stats().hmacFailures, 1u);
}

// ---------------------------------------------------------------------------
// FLEET_RELEASED: each reason takes the player out of the table and reports it.
// ---------------------------------------------------------------------------

void runReleasedReasons() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);

  const std::int64_t a = 101, b = 102, c = 103;
  carryAs(fleet, server, a, 1001, tokenFor(a));
  carryAs(fleet, server, b, 1002, tokenFor(b));
  carryAs(fleet, server, c, 1003, tokenFor(c));
  CHECK_EQ(fleet.carriedCount(), 3u);

  // 1, 2 and 5 are the only reasons the server sends.
  CHECK_EQ(static_cast<int>(FleetReleaseReason::CarriedElsewhere), 1);
  CHECK_EQ(static_cast<int>(FleetReleaseReason::SignInEnded), 2);
  CHECK_EQ(static_cast<int>(FleetReleaseReason::Idle), 5);
  struct Case {
    std::int64_t player;
    std::int64_t user;
    std::uint8_t wire;
    FleetReleaseReason reason;
  };
  const Case cases[] = {
      {a, 1001, 1, FleetReleaseReason::CarriedElsewhere},
      {b, 1002, 2, FleetReleaseReason::SignInEnded},
      {c, 1003, 5, FleetReleaseReason::Idle},
  };
  std::size_t expected = 0;
  for (const Case& r : cases) {
    server.send(own(releasedFrame(r.player, r.wire)));
    ++expected;
    pumpUntil(fleet, [&] { return seen.released.size() == expected; });
    CHECK_EQ(seen.released.size(), expected);
    CHECK_EQ(seen.released.back().playerGameTokenId, r.player);
    CHECK_EQ(seen.released.back().userId, r.user);
    CHECK_EQ(static_cast<int>(seen.released.back().reason), static_cast<int>(r.reason));
    CHECK(!fleet.carries(r.player));
    CHECK(!fleet.player(r.player).has_value());
    SpatialSend p;
    p.uuid = uuid('a');
    CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(r.player, p).error()),
             static_cast<int>(Errc::NotCarried));
  }
  CHECK_EQ(fleet.carriedCount(), 0u);
  CHECK_EQ(fleet.stats().messagesReceivedByType[52], 3u);
  // The session itself stays.
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));

  // A FLEET_RELEASED for a player the table does not hold ends nothing and reports nothing:
  // one for a stranger, and the three above sent again (a captured frame verifies again;
  // the wire carries nothing that makes it fresh). Each is still counted as received.
  server.send(own(releasedFrame(999, 1)));
  server.send(own(releasedFrame(a, 1)));
  server.send(own(releasedFrame(b, 2)));
  server.send(own(releasedFrame(c, 5)));
  pumpUntil(fleet, [&] { return fleet.stats().messagesReceivedByType[52] == 7; });
  settle(fleet);
  CHECK_EQ(fleet.stats().messagesReceivedByType[52], 7u);
  CHECK_EQ(seen.released.size(), 3u);
  CHECK_EQ(fleet.stats().malformed, 0u);
  CHECK_EQ(fleet.stats().hmacFailures, 0u);

  // A reason byte the platform does not send is handed over as Unknown, with the byte
  // beside it, and the carry has ended all the same.
  CHECK_EQ(static_cast<int>(FleetReleaseReason::Unknown), 255);
  for (std::uint8_t odd : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{6}, std::uint8_t{255}}) {
    carryAs(fleet, server, b, 1002, tokenFor(b));
    const std::size_t before = seen.released.size();
    server.send(own(releasedFrame(b, odd)));
    pumpUntil(fleet, [&] { return seen.released.size() == before + 1; });
    CHECK_EQ(seen.released.size(), before + 1);
    CHECK_EQ(static_cast<int>(seen.released.back().reason), static_cast<int>(FleetReleaseReason::Unknown));
    CHECK_EQ(seen.released.back().wireReason, odd);
    CHECK_EQ(seen.released.back().userId, 1002);
    CHECK_EQ(std::string(fleetReleaseReasonName(seen.released.back().reason)), std::string("Unknown"));
    CHECK(!fleet.player(b).has_value());
  }
  CHECK_EQ(seen.released[0].wireReason, 1u);
  CHECK_EQ(seen.released[2].wireReason, 5u);
  CHECK_EQ(std::string(fleetReleaseReasonName(seen.released[2].reason)), std::string("Idle"));
  const std::size_t reported = seen.released.size();  // 3 known and 5 unknown

  // A crossing back: carried again, then released while a carry with a refreshed token
  // waits. The waiting carry keeps its row and still reports its own result.
  carryAs(fleet, server, a, 1001, tokenFor(a));
  CHECK(fleet.carry({tokenFor(a), a}).ok());
  readCarry(server.recvOne(), tokenFor(a));
  server.send(own(releasedFrame(a, 2)));
  pumpUntil(fleet, [&] { return seen.released.size() == reported + 1; });
  CHECK(!fleet.carries(a));
  CHECK(fleet.player(a).has_value());
  CHECK(fleet.player(a)->pending);
  const std::size_t results = seen.results.size();
  server.send(own(carryResultFrame(a, 0, 1)));
  settle(fleet);
  CHECK_EQ(seen.results.size(), results);  // asked once more first
  clock.advance(250);
  fleet.pump(0);
  readCarry(server.recvOne(), tokenFor(a));
  server.send(own(carryResultFrame(a, 0, 1)));
  pumpUntil(fleet, [&] { return seen.results.size() > results; });
  CHECK_EQ(static_cast<int>(seen.results.back().status), static_cast<int>(FleetCarryStatus::NoLiveToken));
  CHECK(!fleet.player(a).has_value());

  // Without its HMAC a release is no frame; with another key's it is dropped and counted.
  server.send(releasedFrame(b, 1));
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 1; });
  server.send(own(releasedFrame(b, 1), playerKey()));
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  CHECK_EQ(fleet.stats().malformed, 1u);
  CHECK_EQ(fleet.stats().hmacFailures, 1u);
  CHECK_EQ(seen.released.size(), reported + 1);
}

// ---------------------------------------------------------------------------
// A refreshed player token: the new carry replaces the same user's older entry once its
// result arrives (the player continues under the new token id).
// ---------------------------------------------------------------------------

void runRefreshedTokenReplacesEntry() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);

  const std::int64_t oldId = 201, newId = 202, user = 2001;
  carryAs(fleet, server, oldId, user, tokenFor(oldId));
  CHECK(fleet.setSpeakers(oldId, SpeakerList::only({uuid('s')}), SpeakerList::keep()).ok());
  CHECK_EQ(server.recvOne()[0], 47u);

  // The carry with the new token is under way: the old entry still sends.
  CHECK(fleet.carry({tokenFor(newId), newId}).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(newId)).player, newId);
  CHECK(fleet.carries(oldId));
  CHECK(!fleet.carries(newId));
  SpatialSend p;
  p.uuid = uuid('a');
  CHECK(fleet.sendActorUpdate(oldId, p).ok());
  CHECK_EQ(server.recvOne()[0], 128u);
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(newId, p).error()), static_cast<int>(Errc::NotCarried));

  // Its result, for the same user: the new entry replaces the old one.
  server.send(own(carryResultFrame(newId, user, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(newId); });
  CHECK(fleet.carries(newId));
  CHECK(!fleet.carries(oldId));
  CHECK(!fleet.player(oldId).has_value());
  CHECK_EQ(fleet.players().size(), 1u);
  CHECK_EQ(fleet.carriedCount(), 1u);
  CHECK_EQ(fleet.player(newId)->userId, user);
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(oldId, p).error()), static_cast<int>(Errc::NotCarried));
  // The player's speaker list came along and is sent under the new id.
  {
    auto got = server.recvOne();
    CHECK_EQ(got[0], 47u);
    CHECK_EQ(tailId(got), newId);
  }
  CHECK(fleet.sendActorUpdate(newId, p).ok());
  {
    auto got = server.recvOne();
    CHECK_EQ(got[0], 128u);
    CHECK_EQ(tailId(got), newId);
  }

  // Another user's carry replaces nothing.
  carryAs(fleet, server, 203, 2002, tokenFor(203));
  CHECK(fleet.carries(newId));
  CHECK_EQ(fleet.carriedCount(), 2u);

  // A new token under the same gameTokenId: the player stays carried while it is confirmed,
  // and the proof is the new token's.
  std::string sameIdToken = tokenFor(newId);
  sameIdToken[0] = 'q';
  CHECK(fleet.carry({sameIdToken, newId}).ok());
  CHECK_EQ(readCarry(server.recvOne(), sameIdToken).player, newId);
  CHECK(fleet.carries(newId));
  CHECK(fleet.player(newId)->pending);
  server.send(own(carryResultFrame(newId, user, 0)));
  pumpUntil(fleet, [&] { return !fleet.player(newId)->pending; });
  CHECK(fleet.carries(newId));
  CHECK_EQ(fleet.carriedCount(), 2u);
}

// ---------------------------------------------------------------------------
// What arrives plain and what arrives in a FLEET_DELIVERY, and who each is for.
// ---------------------------------------------------------------------------

void runDeliveryAndPlainState() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);

  std::vector<Note> any, actorUpdates, voxels, texts, audio, video, direct, events, generics, channelMessages,
      channelAudio;
  FleetHandlers h;
  h.any = spatialInto(any);
  h.actorUpdate = spatialInto(actorUpdates);
  h.text = spatialInto(texts);
  h.audio = spatialInto(audio);
  h.video = spatialInto(video);
  h.singleActorMessage = spatialInto(direct);
  h.genericSpatial = spatialInto(generics);
  h.voxelUpdate = [&](const SpatialNotification& n, const wire::VoxelPayloadView& v, FleetRecipients r) {
    voxels.push_back({static_cast<std::uint8_t>(n.type), r.forSession(), {r.begin(), r.end()},
                      {v.state.begin(), v.state.end()}, v.voxelType, n.sequence});
  };
  h.clientEvent = [&](const SpatialNotification& n, const wire::EventPayloadView& e, FleetRecipients r) {
    events.push_back({static_cast<std::uint8_t>(n.type), r.forSession(), {r.begin(), r.end()},
                      {e.state.begin(), e.state.end()}, e.eventType, n.sequence});
  };
  h.channelMessage = channelInto(channelMessages, 18);
  h.channelAudio = channelInto(channelAudio, 36);
  fleet.setHandlers(std::move(h));
  openConnected(fleet, server);

  const std::int64_t a = 301, b = 302;
  carryAs(fleet, server, a, 3001, tokenFor(a));
  carryAs(fleet, server, b, 3002, tokenFor(b));

  const std::uint8_t body[] = {'h', 'i'};
  const Bytes bytes(body, sizeof(body));
  std::uint8_t eventPayload[wire::kEventTypeSize + 2];
  CHECK(wire::encodeEventPayload(0x0304, bytes, MutableBytes(eventPayload, sizeof(eventPayload))).ok());
  const Bytes eventBytes(eventPayload, sizeof(eventPayload));

  // State arrives plain, once, for the session: the game server decides which of its
  // players see it.
  server.send(notification(wire::MessageType::ActorUpdateNotification, bytes, 1, fleetKey()));
  pumpUntil(fleet, [&] { return !actorUpdates.empty(); });
  settle(fleet);
  CHECK_EQ(actorUpdates.size(), 1u);
  CHECK_EQ(any.size(), 1u);
  CHECK(actorUpdates[0].forSession);
  CHECK(actorUpdates[0].players.empty());
  CHECK(any[0].forSession);
  CHECK_EQ(actorUpdates[0].payload.size(), 2u);
  // A voxel edit, in a signed bundle beside an event and an app-defined message: once each.
  // A plain 138 or 140 means every carried player in range may receive it.
  std::uint8_t voxelPayload[wire::voxel::kFixedSize + 2];
  CHECK(wire::encodeVoxelPayload(1, 2, 3, 9, bytes, MutableBytes(voxelPayload, sizeof(voxelPayload))).ok());
  server.send(signedBundle(
      {unsignedNote(wire::MessageType::VoxelUpdateNotification, Bytes(voxelPayload, sizeof(voxelPayload)), 2),
       unsignedNote(wire::MessageType::ClientEventNotification, eventBytes, 3),
       unsignedNote(wire::MessageType::GenericSpatial1, bytes, 4)}));
  pumpUntil(fleet, [&] { return !generics.empty(); });
  settle(fleet);
  CHECK_EQ(voxels.size(), 1u);
  CHECK_EQ(events.size(), 1u);
  CHECK_EQ(generics.size(), 1u);
  CHECK(voxels[0].forSession && events[0].forSession && generics[0].forSession);
  CHECK(events[0].players.empty() && generics[0].players.empty());
  CHECK_EQ(voxels[0].detail, 9);
  CHECK_EQ(events[0].detail, 0x0304);
  CHECK_EQ(any.size(), 4u);
  CHECK_EQ(fleet.stats().signedBundlesReceived, 1u);
  // State keyed with the player's token is not the session's: dropped.
  server.send(notification(wire::MessageType::ActorUpdateNotification, bytes, 5, playerKey()));
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  CHECK_EQ(fleet.stats().hmacFailures, 1u);
  CHECK_EQ(actorUpdates.size(), 1u);

  // The same event and app-defined message inside a FLEET_DELIVERY: only the listed players.
  server.send(own(deliveryFrame({b}, unsignedNote(wire::MessageType::ClientEventNotification, eventBytes, 6))));
  server.send(own(deliveryFrame({a}, unsignedNote(wire::MessageType::GenericSpatial1, bytes, 7))));
  pumpUntil(fleet, [&] { return events.size() == 2 && generics.size() == 2; });
  CHECK_EQ(events.size(), 2u);
  CHECK(!events[1].forSession);
  CHECK(events[1].players == std::vector<std::int64_t>({b}));
  CHECK_EQ(events[1].detail, 0x0304);
  CHECK(!generics[1].forSession);
  CHECK(generics[1].players == std::vector<std::int64_t>({a}));

  // Communications always arrive in a FLEET_DELIVERY: spatial text for two carried players.
  server.send(own(deliveryFrame({a, b}, unsignedNote(wire::MessageType::ClientTextNotification, bytes, 8))));
  pumpUntil(fleet, [&] { return !texts.empty(); });
  settle(fleet);
  CHECK_EQ(texts.size(), 1u);
  CHECK(!texts[0].forSession);
  CHECK(texts[0].players == std::vector<std::int64_t>({a, b}));
  CHECK_EQ(texts[0].payload.size(), 2u);
  CHECK_EQ(texts[0].payload[0], 'h');
  CHECK(!any.back().forSession);
  CHECK(any.back().players == std::vector<std::int64_t>({a, b}));
  CHECK_EQ(any.back().type, 137u);

  // Voice for one player, video and a direct message: each through its typed handler.
  server.send(own(deliveryFrame({b}, unsignedNote(wire::MessageType::ClientAudioNotification, bytes, 9))));
  server.send(own(deliveryFrame({a}, unsignedNote(wire::MessageType::ClientVideoNotification, bytes, 10))));
  server.send(own(deliveryFrame({a}, unsignedNote(wire::MessageType::SingleActorMessage, bytes, 11))));
  pumpUntil(fleet, [&] { return !audio.empty() && !video.empty() && !direct.empty(); });
  CHECK_EQ(audio.size(), 1u);
  CHECK(audio[0].players == std::vector<std::int64_t>({b}));
  CHECK_EQ(video.size(), 1u);
  CHECK(video[0].players == std::vector<std::int64_t>({a}));
  CHECK_EQ(direct.size(), 1u);
  CHECK(direct[0].players == std::vector<std::int64_t>({a}));

  // 32 recipients, the most one frame lists, around a channel message.
  std::vector<std::int64_t> everyone;
  for (std::int64_t i = 0; i < 32; ++i) everyone.push_back(7000 + i);
  server.send(own(deliveryFrame(everyone, channelNote(18, 55, bytes))));
  pumpUntil(fleet, [&] { return !channelMessages.empty(); });
  CHECK_EQ(channelMessages.size(), 1u);
  CHECK(!channelMessages[0].forSession);
  CHECK_EQ(channelMessages[0].players.size(), 32u);
  CHECK(channelMessages[0].players == everyone);
  CHECK_EQ(channelMessages[0].detail, 55);
  CHECK_EQ(channelMessages[0].payload.size(), 2u);
  // Channel audio.
  server.send(own(deliveryFrame({a, b}, channelNote(36, 56, bytes))));
  pumpUntil(fleet, [&] { return !channelAudio.empty(); });
  CHECK_EQ(channelAudio.size(), 1u);
  CHECK(channelAudio[0].players == std::vector<std::int64_t>({a, b}));
  CHECK_EQ(channelAudio[0].detail, 56);
  CHECK_EQ(channelMessages.size(), 1u);
  CHECK_EQ(fleet.stats().malformed, 0u);
  CHECK_EQ(fleet.stats().hmacFailures, 1u);

  // Malformed counts: 0, 33, and a count longer than the frame. Nothing is dispatched.
  const auto text = unsignedNote(wire::MessageType::ClientTextNotification, bytes, 12);
  const std::size_t textsBefore = texts.size();
  const std::size_t anyBefore = any.size();
  server.send(own(deliveryFrame(0, {}, text)));
  std::vector<std::int64_t> thirtyThree(33, a);
  server.send(own(deliveryFrame(33, thirtyThree, text)));
  server.send(own(deliveryFrame(30, {a, b}, text)));  // 30 named, 2 listed
  server.send(own(deliveryFrame(2, {a, b}, {})));     // the list and no notification
  // A notification that is no notification: a client-only type, an unknown one, one that
  // claims an HMAC of its own (inside a delivery it is always the unsigned form), and a
  // channel notification with bytes after it.
  server.send(own(deliveryFrame({a}, unsignedNote(wire::MessageType::ClientCapabilities, bytes, 13))));
  server.send(own(deliveryFrame({a}, {0x63, 1, 2, 3})));
  server.send(own(deliveryFrame({a}, notification(wire::MessageType::ClientTextNotification, bytes, 14, fleetKey()))));
  auto trailing = channelNote(18, 55, bytes);
  trailing.push_back(0);
  server.send(own(deliveryFrame({a}, trailing)));
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 8; });
  settle(fleet);
  CHECK_EQ(fleet.stats().malformed, 8u);
  CHECK_EQ(texts.size(), textsBefore);
  CHECK_EQ(any.size(), anyBefore);
  CHECK_EQ(channelMessages.size(), 1u);

  // A delivery signed with another key, one changed in flight, and one with no HMAC at all.
  server.send(own(deliveryFrame({a}, text), playerKey()));
  auto tampered = own(deliveryFrame({a}, text));
  tampered[2 + 8 + wire::offsets::kPayload] ^= 0xff;
  server.send(tampered);
  server.send(deliveryFrame({a}, text));
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 4; });
  settle(fleet);
  CHECK_EQ(fleet.stats().hmacFailures, 4u);
  CHECK_EQ(texts.size(), textsBefore);

  // A plain Connection takes none of the three fleet messages, as before they existed.
  struct OneServer final : ISessionProvider {
    int port;
    explicit OneServer(int p) : port(p) {}
    Result<Assignment> assignServer() override { return Assignment{"127.0.0.1", "", port}; }
    Result<TokenInfo> refreshToken() override { return Errc::Rejected; }
  };
  FakeServer other;
  other.start();
  Config plain;
  plain.appId = kAppId;
  plain.token = TokenInfo{kFleetToken, kFleetId, 0};
  plain.manualPump = true;
  plain.sessionReadyWaitMs = 0;
  plain.bundleSends = false;
  plain.advertiseCapabilities = false;
  Connection conn(plain, std::make_shared<OneServer>(other.port), crypto());
  int plainEvents = 0;
  Handlers plainHandlers;
  plainHandlers.any = [&](const SpatialNotification&) { ++plainEvents; };
  plainHandlers.channelMessage = [&](const ChannelNotification&) { ++plainEvents; };
  conn.setHandlers(std::move(plainHandlers));
  CHECK(conn.connect().ok());
  conn.pump(0);
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  other.recvOne();
  other.send(own(carryResultFrame(a, 3001, 0)));
  other.send(own(releasedFrame(a, 1)));
  other.send(own(deliveryFrame({a}, text)));
  other.send(signedBundle({deliveryFrame({a}, channelNote(18, 55, bytes))}));
  for (int i = 0; i < 100 && conn.stats().datagramsReceived < 4; ++i) {
    conn.pump(20);
    conn.poll();
  }
  conn.poll();
  CHECK_EQ(conn.stats().datagramsReceived, 4u);
  CHECK_EQ(conn.stats().messagesReceived, 0u);
  CHECK_EQ(conn.stats().malformed, 0u);
  CHECK_EQ(conn.stats().hmacFailures, 0u);
  CHECK_EQ(plainEvents, 0);
}

// ---------------------------------------------------------------------------
// 51, 52 and 53 come in two forms, and a session reads both whatever it advertised:
// followed by their own HMAC (alone, or in a plain MESSAGE_BUNDLE), or bare inside a
// MESSAGE_BUNDLE_SIGNED. Nothing else is taken.
// ---------------------------------------------------------------------------

void runBothSignedForms() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  std::vector<Note> texts;
  FleetHandlers h = recording(seen);
  h.text = spatialInto(texts);
  fleet.setHandlers(std::move(h));
  openConnected(fleet, server);  // this session advertised BUNDLE_SIGNED (flags 0x03)

  const std::int64_t a = 501, b = 502, c = 503;
  const std::uint8_t body[] = {'h', 'i'};
  const auto text = unsignedNote(wire::MessageType::ClientTextNotification, Bytes(body, 2), 1);
  for (std::int64_t id : {a, b, c}) {
    CHECK(fleet.carry({tokenFor(id), id}).ok());
    readCarry(server.recvOne(), tokenFor(id));
  }

  // Without an HMAC of its own and outside a signed bundle, a frame is taken on nobody's
  // word: dropped, and the carries still wait.
  server.send(carryResultFrame(a, 5001, 0));                      // 18 bytes, alone
  server.send(releasedFrame(a, 1));                               // 10 bytes, alone
  server.send(plainBundle({carryResultFrame(a, 5001, 0)}));       // bare in a plain bundle
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 3; });
  server.send(deliveryFrame({a}, text));                          // its last 32 bytes are no HMAC
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  settle(fleet);
  CHECK_EQ(fleet.stats().malformed, 3u);
  CHECK_EQ(fleet.stats().hmacFailures, 1u);
  CHECK(seen.results.empty() && seen.released.empty() && texts.empty());
  CHECK(fleet.player(a)->pending && !fleet.carries(a));

  // Form one, alone: the frame and its own HMAC.
  server.send(own(carryResultFrame(a, 5001, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(a); });
  CHECK(fleet.carries(a));
  CHECK_EQ(seen.results.size(), 1u);

  // Form one, as members of a plain MESSAGE_BUNDLE whose length prefixes count the HMACs.
  server.send(plainBundle({own(carryResultFrame(b, 5002, 0)), own(deliveryFrame({a}, text)),
                           own(releasedFrame(a, 1))}));
  pumpUntil(fleet, [&] { return seen.released.size() == 1; });
  CHECK(fleet.carries(b));
  CHECK_EQ(seen.results.size(), 2u);
  CHECK_EQ(texts.size(), 1u);
  CHECK(texts[0].players == std::vector<std::int64_t>({a}));
  CHECK_EQ(seen.released.size(), 1u);
  CHECK(!fleet.carries(a));
  // One member whose HMAC does not hold is dropped; the others of the bundle are taken.
  auto bad = own(releasedFrame(b, 2));
  bad[5] ^= 0x01;
  server.send(plainBundle({bad, own(deliveryFrame({b}, text))}));
  pumpUntil(fleet, [&] { return texts.size() == 2; });
  CHECK_EQ(fleet.stats().hmacFailures, 2u);
  CHECK(fleet.carries(b));
  CHECK_EQ(seen.released.size(), 1u);

  // Form two: bare members of a MESSAGE_BUNDLE_SIGNED, vouched for by its one HMAC.
  server.send(signedBundle({carryResultFrame(c, 5003, 0), deliveryFrame({b, c}, text), releasedFrame(b, 5)}));
  pumpUntil(fleet, [&] { return seen.released.size() == 2; });
  CHECK(fleet.carries(c));
  CHECK_EQ(seen.results.size(), 3u);
  CHECK_EQ(texts.size(), 3u);
  CHECK(texts[2].players == std::vector<std::int64_t>({b, c}));
  CHECK_EQ(static_cast<int>(seen.released[1].reason), static_cast<int>(FleetReleaseReason::Idle));
  CHECK(!fleet.carries(b));
  CHECK_EQ(fleet.stats().signedBundlesReceived, 1u);

  // A signed bundle whose HMAC does not hold is dropped whole.
  auto wrong = signedBundle({releasedFrame(c, 1)});
  wrong.back() ^= 0x01;
  server.send(wrong);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 3; });
  CHECK(fleet.carries(c));
  // Inside a signed bundle a frame has no HMAC of its own: one that brings it is not a frame.
  const std::uint64_t malformed = fleet.stats().malformed;
  server.send(signedBundle({own(releasedFrame(c, 1))}));
  pumpUntil(fleet, [&] { return fleet.stats().malformed == malformed + 1; });
  CHECK_EQ(fleet.stats().malformed, malformed + 1);
  CHECK(fleet.carries(c));
  CHECK_EQ(seen.released.size(), 2u);
}

// ---------------------------------------------------------------------------
// The largest datagram a fleet session is sent: a FLEET_DELIVERY for one player, alone,
// with its own HMAC. 1,242 bytes, ten more than anything an ordinary session is sent.
// ---------------------------------------------------------------------------

void runLargestDatagram() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  std::vector<Note> generics;
  FleetHandlers h;
  h.genericSpatial = spatialInto(generics);
  fleet.setHandlers(std::move(h));
  openConnected(fleet, server);
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);

  std::vector<std::uint8_t> payload(wire::kMaxLongSpatialPayload);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 7 + 3);
  const auto inner = unsignedNote(wire::MessageType::GenericSpatial1, Bytes(payload.data(), payload.size()), 9);
  CHECK_EQ(inner.size(), 1200u);
  const auto largest = own(deliveryFrame({kPlayerId}, inner));
  CHECK_EQ(largest.size(), 1242u);
  CHECK_EQ(largest.size(), wire::fleet::kMaxReceiveDatagramSize);
  CHECK(largest.size() > wire::kMaxDatagramSize);

  const std::uint64_t bytesBefore = fleet.stats().bytesReceived;
  server.send(largest);
  pumpUntil(fleet, [&] { return !generics.empty(); });
  CHECK_EQ(generics.size(), 1u);
  CHECK(!generics[0].forSession);
  CHECK(generics[0].players == std::vector<std::int64_t>({kPlayerId}));
  CHECK(generics[0].payload == payload);
  CHECK_EQ(generics[0].sequence, 9u);
  CHECK_EQ(fleet.stats().bytesReceived - bytesBefore, 1242u);
  CHECK_EQ(fleet.stats().malformed, 0u);
  CHECK_EQ(fleet.stats().hmacFailures, 0u);
  CHECK_EQ(fleet.stats().ringDropped, 0u);

  // The largest that travels inside a signed bundle (1,197 bytes), there.
  std::vector<std::uint8_t> smaller(wire::fleet::kMaxBundledDeliverySize - 2 - 8 - wire::kMinLongSpatialNoHmac);
  const auto bundled = deliveryFrame(
      {kPlayerId}, unsignedNote(wire::MessageType::GenericSpatial1, Bytes(smaller.data(), smaller.size()), 10));
  CHECK_EQ(bundled.size(), 1197u);
  const auto bundle = signedBundle({bundled});
  CHECK_EQ(bundle.size(), wire::kMaxDatagramSize);
  server.send(bundle);
  pumpUntil(fleet, [&] { return generics.size() == 2; });
  CHECK_EQ(generics.size(), 2u);
  CHECK_EQ(generics[1].payload.size(), smaller.size());

  // One byte changed in the largest: its HMAC no longer holds.
  auto changed = largest;
  changed[700] ^= 0x01;
  server.send(changed);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  CHECK_EQ(fleet.stats().hmacFailures, 1u);
  CHECK_EQ(generics.size(), 2u);
}

// ---------------------------------------------------------------------------
// CLIENT_CAPABILITIES: the session's own, always sent, with bit 3 (FLEET_OWN_STATE) when
// asked.
// ---------------------------------------------------------------------------

void runFleetOwnStateCapability() {
  CHECK_EQ(wire::ClientCapability::kFleetOwnState, 8u);
  {
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetConfig cfg = configFor(server);
    cfg.transport.advertiseIntervalMs = 15'000;
    FleetSession fleet(cfg, crypto(), clock);
    CHECK(!fleet.fleetOwnState());
    // Off by default: signed bundles and bound control only.
    openConnected(fleet, server);

    // Each change is advertised at once; the same value sends nothing.
    fleet.setFleetOwnState(true);
    CHECK(fleet.fleetOwnState());
    CHECK_EQ(expectCapabilities(server), 0x0Bu);
    fleet.setFleetOwnState(true);
    server.expectNothing();
    fleet.setFleetOwnState(false);
    CHECK(!fleet.fleetOwnState());
    CHECK_EQ(expectCapabilities(server), 0x03u);
    fleet.setFleetOwnState(false);
    server.expectNothing();

    // Beside the own-media echo: bits 2 and 3 together.
    fleet.setOwnMediaEcho(true);
    CHECK_EQ(expectCapabilities(server), 0x07u);
    fleet.setFleetOwnState(true);
    CHECK_EQ(expectCapabilities(server), 0x0Fu);

    // The periodic advertisement carries it, and each carried player's speaker lists follow
    // under the player's id.
    carryAs(fleet, server, kPlayerId, 77, kPlayerToken);
    CHECK(fleet.setSpeakers(kPlayerId, SpeakerList::only({uuid('s')}), SpeakerList::keep()).ok());
    CHECK_EQ(server.recvOne()[0], 47u);
    clock.advance(cfg.transport.advertiseIntervalMs);
    fleet.pump(0);
    CHECK_EQ(expectCapabilities(server), 0x0Fu);
    auto list = server.recvOne();
    CHECK_EQ(list[0], 47u);
    CHECK_EQ(tailIdSignedByFleet(list), kPlayerId);
  }
  {
    // Asked for from the start.
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetConfig cfg = configFor(server);
    cfg.fleetOwnState = true;
    FleetSession fleet(cfg, crypto(), clock);
    CHECK(fleet.fleetOwnState());
    server.serves(fleet);
    CHECK(fleet.open().ok());
    fleet.pump(0);
    CHECK_EQ(expectCapabilities(server), 0x0Bu);
  }
}

// ---------------------------------------------------------------------------
// A session that comes back (the contract's 8.5).
// ---------------------------------------------------------------------------

/// The nonce a session's CLIENT_CAPABILITIES carries: what a bound control frame signs.
std::vector<std::uint8_t> capabilitiesNonce(FakeServer& server) {
  auto got = nextOfType(server, 29);
  auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
  CHECK(v.ok());
  CHECK_EQ(v->epochMillisOrTokenId, kFleetId);
  CHECK(wire::verifyLongSpatial(crypto(), Bytes(got.data(), got.size()), fleetKey()).ok());
  CHECK_EQ(v->payload.size(), 4u + wire::kControlNonceSize);
  return std::vector<std::uint8_t>(v->payload.begin() + 4, v->payload.end());
}

/// COMMAND_RECONNECT (22) as a fleet session is sent it: signed with the fleet token.
std::vector<std::uint8_t> reconnectFrame() {
  std::vector<std::uint8_t> f(wire::kCommandReconnectSize);
  f[0] = 22;
  CHECK(crypto().hmacSha256(asBytes(kFleetToken), Bytes(f.data(), 1), f.data() + 1));
  return f;
}

/// COMMAND_RECONNECT_BOUND (44) over the session's nonce, signed with the fleet token.
std::vector<std::uint8_t> boundReconnectFrame(const std::vector<std::uint8_t>& nonce) {
  std::vector<std::uint8_t> f = {44};
  f.insert(f.end(), nonce.begin(), nonce.end());
  const std::size_t signedLen = f.size();
  f.resize(signedLen + wire::kHmacTagSize);
  CHECK(crypto().hmacSha256(asBytes(kFleetToken), Bytes(f.data(), signedLen), f.data() + signedLen));
  return f;
}

// Another server: after its capabilities the session sends a new FLEET_CARRY for every
// player, and treats each as carried again only on its 51. The server left hears nothing.
void runAnotherServerCarriesEveryPlayerAgain() {
  FakeServer first, second;
  first.start();
  second.start();
  ManualClock clock;
  FleetConfig cfg = configFor(first);
  int asked = 0;
  int next = 0;
  cfg.reassign = [&]() -> Result<Assignment> {
    ++asked;
    return Assignment{"127.0.0.1", "", next};
  };
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  first.serves(fleet);
  second.serves(fleet);
  CHECK(fleet.open().ok());
  fleet.pump(0);
  CHECK_EQ(asked, 0);  // open() uses the server it was given
  capabilitiesNonce(first);
  CHECK_EQ(fleet.server().clientPort, first.port);
  CHECK_EQ(fleet.sessionEpoch(), 1u);

  const std::int64_t a = 401, b = 402;
  carryAs(fleet, first, a, 4001, tokenFor(a));
  carryAs(fleet, first, b, 4002, tokenFor(b));
  CHECK_EQ(seen.results.size(), 2u);
  SpatialSend p;
  p.uuid = uuid('a');

  // The realtime server tells the session to move: COMMAND_RECONNECT (22), signed with the
  // fleet token. The game server answers with the server its refresh named.
  next = second.port;
  first.send(reconnectFrame());
  pumpUntil(fleet, [&] { return asked == 1; });
  CHECK_EQ(asked, 1);
  CHECK(!fleet.moveOrdered());
  CHECK_EQ(fleet.server().clientPort, second.port);
  CHECK_EQ(fleet.stats().reconnects, 1u);
  // Between the two servers nothing can be sent for a player.
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connecting));
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(a, p).error()), static_cast<int>(Errc::NotConnected));

  // Connected on the second server: its capabilities first, then a FLEET_CARRY for every
  // player, each with a new time and proof.
  clock.advance(100);
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Connected; });
  CHECK_EQ(fleet.sessionEpoch(), 2u);
  auto firstOnSecond = second.recvOne();
  CHECK_EQ(firstOnSecond[0], 29u);
  CHECK_EQ(tailId(firstOnSecond), kFleetId);
  const std::vector<std::uint8_t> nonceOnSecond(firstOnSecond.begin() + wire::offsets::kPayload + 4,
                                                firstOnSecond.begin() + wire::offsets::kPayload + 12);
  std::vector<std::int64_t> again;
  for (int i = 0; i < 2; ++i) {
    auto got = second.recvOne();
    const std::int64_t id = le::readI64(got.data() + wire::offsets::kPayload);
    const Carry c = readCarry(got, tokenFor(id));
    CHECK_EQ(c.epochMs, kEpochMs + 100);
    again.push_back(c.player);
  }
  CHECK((again == std::vector<std::int64_t>({a, b})) || (again == std::vector<std::int64_t>({b, a})));
  // The new server holds no carry: each player is carried again only on its 51.
  CHECK(!fleet.carries(a) && !fleet.carries(b));
  CHECK(fleet.player(a)->pending && fleet.player(b)->pending);
  CHECK_EQ(fleet.player(a)->userId, 4001);
  CHECK_EQ(fleet.carriedCount(), 0u);
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(a, p).error()), static_cast<int>(Errc::NotCarried));
  second.expectNothing();
  // The server it left is sent nothing more: no release, no carry.
  first.expectNothing();

  // The new server confirms one and refuses the other.
  second.send(own(carryResultFrame(a, 4001, 0)));
  second.send(own(carryResultFrame(b, 0, 2)));
  pumpUntil(fleet, [&] { return seen.results.size() == 4; });
  CHECK_EQ(seen.results.size(), 4u);
  CHECK(fleet.carries(a));
  CHECK(!fleet.player(a)->pending);
  CHECK(!fleet.player(b).has_value());
  CHECK_EQ(fleet.carriedCount(), 1u);
  CHECK(fleet.sendActorUpdate(a, p).ok());
  CHECK_EQ(second.recvOne()[0], 128u);

  // The bound form (44), over this assignment's nonce: back to the first server, where the
  // same happens.
  next = first.port;
  second.send(boundReconnectFrame(nonceOnSecond));
  pumpUntil(fleet, [&] { return asked == 2; });
  CHECK_EQ(asked, 2);
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Connected; });
  CHECK_EQ(fleet.sessionEpoch(), 3u);
  capabilitiesNonce(first);
  CHECK_EQ(readCarry(first.recvOne(), tokenFor(a)).player, a);
  CHECK(!fleet.carries(a));
  first.send(own(carryResultFrame(a, 4001, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(a); });
  CHECK(fleet.carries(a));
  CHECK_EQ(seen.results.size(), 5u);

  // A refresh whose answer names another server (applyRefresh): the same, without asking.
  CHECK(fleet.applyRefresh(TokenInfo{kFleetToken, kFleetId, kEpochMs + 86'400'000},
                           Assignment{"127.0.0.1", "", second.port})
            .ok());
  CHECK_EQ(fleet.token().expiresAtEpochMs, kEpochMs + 86'400'000);
  pumpUntil(fleet, [&] { return fleet.sessionEpoch() == 4 && fleet.state() == ConnState::Connected; });
  CHECK_EQ(asked, 2);
  CHECK_EQ(fleet.server().clientPort, second.port);
  CHECK_EQ(second.recvOne()[0], 29u);
  CHECK_EQ(readCarry(second.recvOne(), tokenFor(a)).player, a);
  CHECK(!fleet.carries(a));
  first.expectNothing();

  // A carry again that the new server never answers ends the carry here.
  const std::size_t before = seen.results.size();
  clock.advance(5000);
  fleet.pump(0);
  fleet.poll();
  CHECK_EQ(seen.results.size(), before + 1);
  CHECK_EQ(static_cast<int>(seen.results.back().status), static_cast<int>(FleetCarryStatus::TimedOut));
  CHECK(!fleet.player(a).has_value());

  // A server without an address is refused, and naming the server it is on does nothing.
  CHECK_EQ(static_cast<int>(fleet.setServer(Assignment{"", "", first.port}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK_EQ(static_cast<int>(fleet.setServer(Assignment{"127.0.0.1", "", 0}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK(fleet.setServer(Assignment{"127.0.0.1", "", second.port}).ok());
  settle(fleet);
  CHECK_EQ(fleet.sessionEpoch(), 4u);
}

// The same server: after a refresh the carries stand. The session sends its capabilities
// again and no FLEET_CARRY.
void runSameServerKeepsItsCarries() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  int asked = 0;
  cfg.reassign = [&]() -> Result<Assignment> {
    ++asked;  // the game server refreshed, and the answer named the server the session is on
    return Assignment{"127.0.0.1", "", server.port};
  };
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);
  const std::int64_t a = 601, b = 602;
  carryAs(fleet, server, a, 6001, tokenFor(a));
  carryAs(fleet, server, b, 6002, tokenFor(b));
  SpatialSend p;
  p.uuid = uuid('a');

  // A refresh that names the server the session is on: the new expiry, the capabilities
  // again, and nothing else.
  CHECK(fleet.applyRefresh(TokenInfo{kFleetToken, kFleetId, kEpochMs + 86'400'000},
                           Assignment{"127.0.0.1", "", server.port})
            .ok());
  CHECK_EQ(fleet.token().expiresAtEpochMs, kEpochMs + 86'400'000);
  CHECK_EQ(expectCapabilities(server), 0x03u);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();
  CHECK_EQ(fleet.sessionEpoch(), 1u);
  CHECK(fleet.carries(a) && fleet.carries(b));
  CHECK(!fleet.player(a)->pending && !fleet.player(b)->pending);
  CHECK_EQ(fleet.stats().messagesSentByType[49], 2u);
  CHECK(fleet.sendActorUpdate(a, p).ok());
  CHECK_EQ(server.recvOne()[0], 128u);
  // setToken alone (the same token and id, a new expiry) sends nothing.
  CHECK(fleet.setToken(TokenInfo{kFleetToken, kFleetId, kEpochMs + 90'000'000}).ok());
  fleet.pump(0);
  server.expectNothing();

  // The same server from a new address (the session reconnected, and its game server's
  // refresh named the same server): the capabilities again, and no FLEET_CARRY.
  server.send(reconnectFrame());
  pumpUntil(fleet, [&] { return asked == 1; });
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Connected; });
  CHECK_EQ(fleet.sessionEpoch(), 2u);
  CHECK_EQ(expectCapabilities(server), 0x03u);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();
  CHECK(fleet.carries(a) && fleet.carries(b));
  CHECK_EQ(fleet.stats().messagesSentByType[49], 2u);
  CHECK_EQ(seen.results.size(), 2u);
  CHECK(fleet.sendActorUpdate(b, p).ok());
  {
    auto got = server.recvOne();
    CHECK_EQ(got[0], 128u);
    CHECK_EQ(tailId(got), b);
  }
}

// A move order nobody answers inside the session: it says so, stays where it is with its
// carries, and goes where the next refresh says.
void runMoveOrderWaitsForARefresh() {
  FakeServer first, second;
  first.start();
  second.start();
  ManualClock clock;
  FleetSession fleet(configFor(first), crypto(), clock);  // no FleetConfig::reassign
  openConnected(fleet, first);
  second.serves(fleet);
  const std::int64_t a = 701;
  carryAs(fleet, first, a, 7001, tokenFor(a));
  CHECK(!fleet.moveOrdered());
  CHECK(!fleet.refreshDue());

  first.send(reconnectFrame());
  pumpUntil(fleet, [&] { return fleet.moveOrdered(); });
  CHECK(fleet.moveOrdered());
  CHECK(fleet.refreshDue());
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Connected; });
  CHECK_EQ(fleet.server().clientPort, first.port);
  CHECK_EQ(expectCapabilities(first), 0x03u);  // again, from its new address
  fleet.pump(0);
  first.expectNothing();  // and no FLEET_CARRY: the same server
  CHECK(fleet.carries(a));

  // The refresh names another server: there, capabilities and every player again.
  CHECK(fleet.applyRefresh(TokenInfo{kFleetToken, kFleetId, 0}, Assignment{"127.0.0.1", "", second.port}).ok());
  CHECK(!fleet.moveOrdered());
  pumpUntil(fleet, [&] { return fleet.sessionEpoch() == 3 && fleet.state() == ConnState::Connected; });
  CHECK_EQ(expectCapabilities(second), 0x03u);
  CHECK_EQ(readCarry(second.recvOne(), tokenFor(a)).player, a);
  CHECK(!fleet.carries(a));
  second.send(own(carryResultFrame(a, 7001, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(a); });
  CHECK(fleet.carries(a));
}

// ---------------------------------------------------------------------------
// The silence timeout. The realtime server tells a session that it ended in one datagram,
// which can be lost, and a session never authorized on its server is sent nothing: no error
// frame, no acknowledgement of its capabilities. (The build before v0.50.1 told a session
// closed through the Application API nothing at all.) So a FleetConfig starts with
// FleetConfig::transport.watchdogSilenceMs 45,000:
// nothing the server signed for that long makes a refresh due, and the refresh's answer
// (NOT_FOUND for an ended session) is what the game server acts on.
// ---------------------------------------------------------------------------

/// CONTROL_BOUND_ACK (46) for a CLIENT_CAPABILITIES the session sent: over its nonce, signed
/// with the fleet token. What a live session hears every capabilities interval.
std::vector<std::uint8_t> acknowledgementOf(const std::vector<std::uint8_t>& capabilities) {
  auto v = wire::parseLongSpatial(Bytes(capabilities.data(), capabilities.size()));
  CHECK(v.ok());
  CHECK_EQ(v->payload.size(), 4u + wire::kControlNonceSize);
  std::vector<std::uint8_t> f = {46};
  f.insert(f.end(), v->payload.begin() + 4, v->payload.end());
  const std::size_t signedLen = f.size();
  f.resize(signedLen + wire::kHmacTagSize);
  CHECK(crypto().hmacSha256(asBytes(kFleetToken), Bytes(f.data(), signedLen), f.data() + signedLen));
  CHECK_EQ(f.size(), wire::kControlBoundAckSize);
  return f;
}

/// Everything the session has sent that the server has not read yet.
std::vector<std::vector<std::uint8_t>> sentSoFar(FakeServer& server) { return server.takeAll(); }

void runSilenceTimeoutDefaults() {
  // On for a fleet session, off for a player's connection, as it always was.
  CHECK_EQ(FleetConfig{}.transport.watchdogSilenceMs, 45'000);
  CHECK_EQ(kFleetSilenceTimeoutMs, 45'000);
  CHECK_EQ(fleetTransportDefaults().watchdogSilenceMs, 45'000);
  CHECK_EQ(fleetTransportDefaults().advertiseIntervalMs, 15'000);
  CHECK_EQ(Config{}.watchdogSilenceMs, 0);

  // The capabilities interval is at most a third of the timeout, and a timeout below 3,000
  // is 3,000. Counted on the wire: an acknowledged session's advertisements in 12 seconds.
  struct Case {
    std::int64_t timeoutMs, intervalMs;
    int expected;
  };
  for (const Case c : {Case{6'000, 3'600'000, 7},   // every 2,000
                       Case{1, 3'600'000, 13},      // 3,000: every 1,000
                       Case{45'000, 4'000, 4},      // a shorter interval is kept
                       Case{0, 5'000, 3}}) {        // off: the interval is as it was set
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetConfig cfg = configFor(server);
    cfg.transport.watchdogSilenceMs = c.timeoutMs;
    cfg.transport.advertiseIntervalMs = c.intervalMs;
    FleetSession fleet(cfg, crypto(), clock);
    server.serves(fleet);
    CHECK(fleet.open().ok());
    int advertisements = 0;
    for (int step = 0; step <= 48; ++step) {  // 250 ms steps
      fleet.pump(0);
      for (const auto& got : sentSoFar(server)) {
        CHECK_EQ(got[0], 29u);
        ++advertisements;
        server.send(acknowledgementOf(got));
      }
      fleet.pump(0);
      fleet.poll();
      CHECK(!fleet.moveOrdered());
      clock.advance(250);
    }
    CHECK_EQ(advertisements, c.expected);
  }
}

// Every allowed timeout fires, within one capabilities interval of its value, when the
// server says nothing from the start (OI-2026-11-27-001: at the minimum the capabilities
// interval is a third of the timeout, 1,000 ms, which was also how long the check waited for
// the acknowledgement of the capabilities that last left; an advertisement left every time
// that wait was over, so a timeout of 3,000 to about 3,300 never fired). The session is
// pumped every 10 ms, as a game's loop would.
void runEverySilenceTimeoutFires() {
  struct Case {
    std::int64_t asked, timeoutMs, intervalMs;
  };
  for (const Case c : {Case{3'000, 3'000, 1'000},      // the minimum
                       Case{1, 3'000, 1'000},          // below it: raised to it
                       Case{3'002, 3'002, 1'000},
                       Case{3'300, 3'300, 1'100},
                       Case{4'500, 4'500, 1'500},
                       Case{45'000, 45'000, 15'000}}) {  // the default
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetConfig cfg = watchedConfigFor(server);
    cfg.transport.watchdogSilenceMs = c.asked;
    FleetSession fleet(cfg, crypto(), clock);
    server.serves(fleet);
    CHECK(fleet.open().ok());
    const std::int64_t start = clock.monotonicMillis();
    std::int64_t firedAtMs = -1;
    for (int step = 0; step <= 7'000 && firedAtMs < 0; ++step) {  // 70 seconds at most
      fleet.pump(0);
      fleet.poll();
      if (fleet.moveOrdered()) {
        firedAtMs = clock.monotonicMillis() - start;
        CHECK(fleet.refreshDue());
      } else {
        clock.advance(10);
      }
    }
    if (firedAtMs < 0) {
      std::fprintf(stderr, "a silence timeout of %lld ms did not fire in 70 seconds\n",
                   static_cast<long long>(c.asked));
    }
    // After its value, and no later than the first pass a capabilities interval after it.
    CHECK(firedAtMs > c.timeoutMs);
    CHECK(firedAtMs <= c.timeoutMs + c.intervalMs + 10);
    // Every advertisement on the way was the session's own, a capabilities interval apart.
    int advertisements = 0;
    for (const auto& got : sentSoFar(server)) {
      CHECK_EQ(got[0], 29u);
      ++advertisements;
    }
    CHECK_EQ(advertisements, static_cast<int>(firedAtMs / c.intervalMs) + 1);
    CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
    CHECK_EQ(fleet.stats().reconnects, 0u);
  }
}

// A live session that carries nobody, with nothing in range, hears only the acknowledgements
// of its capabilities. Ten minutes of that never make a refresh due.
void runAnIdleSessionIsNeverTakenForGone() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(watchedConfigFor(server), crypto(), clock);
  server.serves(fleet);
  CHECK(fleet.open().ok());
  int advertisements = 0;
  for (int second = 0; second <= 600; ++second) {
    fleet.pump(0);
    for (const auto& got : sentSoFar(server)) {
      CHECK_EQ(got[0], 29u);  // an idle session sends nothing else
      ++advertisements;
      server.send(acknowledgementOf(got));
    }
    fleet.pump(0);
    fleet.poll();
    CHECK(!fleet.moveOrdered());
    CHECK(!fleet.refreshDue());
    CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
    clock.advance(1000);
  }
  CHECK_EQ(advertisements, 41);  // at 0 and every 15 seconds
  // Every acknowledgement has arrived: server.send() returned when the session had it.
  const auto stats = fleet.stats();
  CHECK_EQ(stats.boundControlAcks, 41u);
  CHECK_EQ(stats.reconnects, 0u);
  CHECK_EQ(stats.messagesReceived, 0u);  // nothing but the acknowledgements arrived
}

// The acknowledgements of two advertisements in a row are lost, again and again, and the
// third's arrives late, after the 45 seconds have passed: no refresh is due. Three lost in a
// row are the timeout: one refresh is due, the session keeps its socket and its state, and
// says so once.
void runTwoLostAcknowledgementsAreNotSilence() {
  FakeServer server;
  server.start();
  ManualClock clock;
  RecordingLogger logger;
  FleetSession fleet(watchedConfigFor(server), crypto(), clock, logger);
  server.serves(fleet);
  CHECK(fleet.open().ok());
  const std::int64_t start = clock.monotonicMillis();
  int advertisements = 0;
  bool acknowledging = true;
  std::int64_t lastHeardMs = -1, trippedAtMs = -1;
  // An acknowledgement the server sends later: (when, frame).
  std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> late;
  for (int step = 0; step <= 4 * 600; ++step) {  // 250 ms steps, ten minutes
    const std::int64_t now = clock.monotonicMillis() - start;
    if (now == 300'000) acknowledging = false;  // from five minutes on the server says nothing
    fleet.pump(0);
    for (const auto& got : sentSoFar(server)) {
      CHECK_EQ(got[0], 29u);
      const int index = advertisements++;
      if (!acknowledging || index % 3 != 0) continue;  // two of every three are lost
      // Every other one that is answered is answered 750 ms late.
      if ((index / 3) % 2 == 1) {
        late.emplace_back(now + 750, acknowledgementOf(got));
      } else {
        server.send(acknowledgementOf(got));
        lastHeardMs = now;
      }
    }
    for (auto it = late.begin(); it != late.end();) {
      if (it->first > now) {
        ++it;
        continue;
      }
      server.send(it->second);
      lastHeardMs = now;
      it = late.erase(it);
    }
    fleet.pump(0);
    fleet.poll();
    if (acknowledging) {
      CHECK(!fleet.moveOrdered());
      CHECK(!fleet.refreshDue());
    } else if (fleet.moveOrdered() && trippedAtMs < 0) {
      trippedAtMs = now;
    }
    CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
    clock.advance(250);
  }
  // While it was answered: 21 advertisements in five minutes, 7 of them acknowledged, and the
  // late ones arrived 45.75 seconds after the one before was heard.
  CHECK(advertisements >= 41);
  CHECK(fleet.stats().boundControlAcks == 7u);
  // Then nothing: a refresh is due between 45 and 46.25 seconds after the last one heard.
  CHECK(trippedAtMs > lastHeardMs + 45'000);
  CHECK(trippedAtMs <= lastHeardMs + 46'250);
  CHECK(fleet.moveOrdered());
  CHECK(fleet.refreshDue());
  CHECK_EQ(fleet.stats().reconnects, 0u);  // it kept its socket
  CHECK_EQ(fleet.server().clientPort, server.port);
  int said = 0;
  {
    std::lock_guard lock(logger.mutex);
    for (const auto& line : logger.lines) said += line.find("a refresh is due") != std::string::npos;
  }
  CHECK_EQ(said, 1);  // once, for four more minutes of silence
}

// What a silent server means for a session that carries a player. With the timeout off
// (0) nothing ever tells the session. With it on, a refresh is due after 45 seconds; the
// carries stay as they are until the game server answers, and then every player is carried
// again, on the server the session was on too: it may have lost them.
void runSilenceMakesARefreshDue() {
  const std::uint8_t pose[] = {1, 2, 3};
  for (const bool watched : {false, true}) {
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetConfig cfg = watched ? watchedConfigFor(server) : configFor(server);
    if (!watched) CHECK_EQ(cfg.transport.watchdogSilenceMs, 0);
    FleetSession fleet(cfg, crypto(), clock);
    Seen seen;
    fleet.setHandlers(recording(seen));
    openConnected(fleet, server);
    const std::int64_t a = 711;
    carryAs(fleet, server, a, 7011, tokenFor(a));  // the last thing the server says

    // From here the server says nothing. The session goes on sending for its player.
    int secondsToDue = -1;
    for (int second = 1; second <= 120; ++second) {
      clock.advance(1000);
      CHECK(fleet.sendActorUpdate(a, {{1, 2, 3}, uuid('a'), Bytes(pose, sizeof(pose))}).ok());
      fleet.pump(0);
      fleet.poll();
      if (fleet.refreshDue() && secondsToDue < 0) secondsToDue = second;
      CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
      CHECK(fleet.carries(a));
    }
    for (const auto& got : sentSoFar(server)) CHECK(got[0] == 29 || got[0] == 128);  // no FLEET_CARRY
    CHECK_EQ(fleet.stats().reconnects, 0u);
    if (!watched) {
      // Nothing told it: the refresh at half the token's lifetime is all it has.
      CHECK(!fleet.moveOrdered());
      CHECK(!fleet.refreshDue());
      CHECK_EQ(secondsToDue, -1);
      continue;
    }
    CHECK(fleet.moveOrdered());
    CHECK(secondsToDue == 46 || secondsToDue == 47);
    CHECK_EQ(fleet.server().clientPort, server.port);  // where it was: nobody named another

    // The refresh answers the server it is on (the session was only out of reach, or the
    // server was restarted): the capabilities, then the player again with a new proof.
    CHECK(fleet.applyRefresh(TokenInfo{kFleetToken, kFleetId, 0}, Assignment{"127.0.0.1", "", server.port}).ok());
    CHECK(!fleet.moveOrdered());
    CHECK(!fleet.refreshDue());
    CHECK(!fleet.carries(a));  // carried again when its result says so
    fleet.pump(0);
    const auto after = sentSoFar(server);
    CHECK_EQ(after.size(), 2u);
    CHECK_EQ(after[0][0], 29u);
    CHECK_EQ(readCarry(after[1], tokenFor(a)).player, a);
    server.send(own(carryResultFrame(a, 7011, 0)));
    pumpUntil(fleet, [&] { return fleet.carries(a); });
    CHECK(fleet.carries(a));
    CHECK_EQ(seen.results.size(), 2u);
    CHECK_EQ(fleet.stats().reconnects, 0u);

    // The server it was placed on has the timeout to be heard from, from the placement and
    // from each frame of its own after: silent again, a refresh is due again.
    secondsToDue = -1;
    for (int second = 1; second <= 60 && secondsToDue < 0; ++second) {
      clock.advance(1000);
      fleet.pump(0);
      fleet.poll();
      if (fleet.refreshDue()) secondsToDue = second;
    }
    CHECK(secondsToDue == 46 || secondsToDue == 47);
    // A refresh that names another server: the session moves there and carries there.
    FakeServer second;
    second.start();
    second.serves(fleet);
    CHECK(fleet.applyRefresh(TokenInfo{kFleetToken, kFleetId, 0}, Assignment{"127.0.0.1", "", second.port}).ok());
    CHECK(!fleet.moveOrdered());
    pumpUntil(fleet, [&] { return fleet.stats().reconnects == 1 && fleet.state() == ConnState::Connected; });
    fleet.pump(0);
    CHECK_EQ(fleet.server().clientPort, second.port);
    CHECK_EQ(expectCapabilities(second), 0x03u);
    CHECK_EQ(readCarry(second.recvOne(), tokenFor(a)).player, a);
  }
}

// With FleetConfig::reassign the session asks the game server itself when the timeout
// passes, on the network thread, and connects where it is told; there it carries every
// player again, the same server included.
void runSilenceAsksTheGameServerForAServer() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = watchedConfigFor(server);
  int asked = 0;
  cfg.reassign = [&]() -> Result<Assignment> {
    ++asked;
    return Assignment{"127.0.0.1", "", server.port};
  };
  FleetSession fleet(cfg, crypto(), clock);
  openConnected(fleet, server);
  const std::int64_t a = 712;
  carryAs(fleet, server, a, 7012, tokenFor(a));
  for (int second = 1; second <= 44; ++second) {
    clock.advance(1000);
    fleet.pump(0);
    fleet.poll();
  }
  CHECK_EQ(asked, 0);
  (void)sentSoFar(server);
  for (int second = 45; second <= 48; ++second) {
    clock.advance(1000);
    fleet.pump(0);
    fleet.poll();
  }
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Connected && fleet.stats().reconnects == 1; });
  CHECK_EQ(asked, 1);
  CHECK(!fleet.moveOrdered());  // the game server answered inside the session
  CHECK_EQ(fleet.stats().reconnects, 1u);
  fleet.pump(0);
  // The advertisement that went unanswered as the timeout passed, and then, from the new
  // socket, the capabilities and the player again.
  const auto after = sentSoFar(server);
  CHECK_EQ(after.size(), 3u);
  CHECK_EQ(after[0][0], 29u);
  CHECK_EQ(after[1][0], 29u);
  CHECK_EQ(readCarry(after[2], tokenFor(a)).player, a);
  CHECK(!fleet.carries(a));
}

// COMMAND_SESSION_RELEASED ends the session as it ends an ordinary one, and its carries end
// with it, unannounced. Opened again, it holds none: every player is carried again, on
// whichever server.
void runSessionReleasedEndsTheCarries() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);
  const std::int64_t a = 801;
  carryAs(fleet, server, a, 8001, tokenFor(a));

  // The idle reason (5), signed with the fleet token.
  std::vector<std::uint8_t> released(wire::kCommandSessionReleasedSize);
  released[0] = 28;
  released[1] = 5;
  CHECK(crypto().hmacSha256(asBytes(kFleetToken), Bytes(released.data(), 2), released.data() + 2));
  server.send(released);
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Inactive; });
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Inactive));
  fleet.poll();
  CHECK(!fleet.carries(a));
  CHECK(fleet.player(a).has_value());  // still a player the session means to carry
  CHECK(seen.released.empty());        // no FLEET_RELEASED: the carries ended with the session
  SpatialSend p;
  p.uuid = uuid('a');
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(a, p).error()), static_cast<int>(Errc::NotCarried));

  // Opened again on the same server: its capabilities, then the player again.
  CHECK(fleet.open().ok());
  fleet.pump(0);
  CHECK_EQ(expectCapabilities(server), 0x03u);
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(a)).player, a);
  CHECK(!fleet.carries(a));
  server.send(own(carryResultFrame(a, 8001, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(a); });
  CHECK(fleet.carries(a));
}

// ---------------------------------------------------------------------------
// Realtime servers v0.50.1: a fleet session whose token is deauthorized (closed through the
// Application API, its org token revoked, fleet sessions switched off) is sent
// COMMAND_SESSION_RELEASED (45 bound to the connection, or 28) with reason 0, signed, once.
// That frame, and the same one for any reason but idleness (1 stale, 2 the fleet token
// expired, 3 overload, 4 a move), makes a refresh due at once: the session carries nobody,
// holds its carries back, and after the refresh carries every player again.
// ---------------------------------------------------------------------------

/// COMMAND_SESSION_RELEASED_BOUND (45): [45][reason][nonce][HMAC over the first 10 bytes].
std::vector<std::uint8_t> boundSessionReleased(const std::vector<std::uint8_t>& nonce, std::uint8_t reason,
                                               const std::string& token = kFleetToken) {
  std::vector<std::uint8_t> f = {45, reason};
  f.insert(f.end(), nonce.begin(), nonce.end());
  const std::size_t signedLen = f.size();
  f.resize(signedLen + wire::kHmacTagSize);
  CHECK(crypto().hmacSha256(asBytes(token), Bytes(f.data(), signedLen), f.data() + signedLen));
  CHECK_EQ(f.size(), wire::kCommandSessionReleasedBoundSize);
  return f;
}

/// COMMAND_SESSION_RELEASED (28): [28][reason][HMAC over the two bytes].
std::vector<std::uint8_t> legacySessionReleased(std::uint8_t reason) {
  std::vector<std::uint8_t> f(wire::kCommandSessionReleasedSize);
  f[0] = 28;
  f[1] = reason;
  CHECK(crypto().hmacSha256(asBytes(kFleetToken), Bytes(f.data(), 2), f.data() + 2));
  return f;
}

void runTheServerEndsTheSession() {
  struct Case {
    std::uint8_t wire;
    FleetSessionReleaseReason reason;
    const char* name;
  };
  const Case cases[] = {{0, FleetSessionReleaseReason::Deauthorized, "Deauthorized"},
                        {1, FleetSessionReleaseReason::Stale, "Stale"},
                        {2, FleetSessionReleaseReason::TokenExpired, "TokenExpired"},
                        {3, FleetSessionReleaseReason::Overload, "Overload"},
                        {4, FleetSessionReleaseReason::Move, "Move"},
                        {9, FleetSessionReleaseReason::Unknown, "Unknown"}};
  const std::uint8_t pose[] = {1, 2, 3};
  for (const Case& c : cases) {
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetSession fleet(configFor(server), crypto(), clock);  // no FleetConfig::reassign
    Seen seen;
    std::vector<FleetSessionReleased> ended;
    FleetHandlers h = recording(seen);
    h.sessionReleased = [&](const FleetSessionReleased& r) { ended.push_back(r); };
    fleet.setHandlers(std::move(h));
    server.serves(fleet);
    CHECK(fleet.open().ok());
    fleet.pump(0);
    const auto nonce = capabilitiesNonce(server);
    const std::int64_t a = 811, b = 812, waiting = 813, later = 814;
    carryAs(fleet, server, a, 8011, tokenFor(a));
    carryAs(fleet, server, b, 8012, tokenFor(b));
    CHECK(fleet.carry({tokenFor(waiting), waiting}).ok());  // one whose answer has not come
    readCarry(server.recvOne(), tokenFor(waiting));
    const std::size_t resultsBefore = seen.results.size();

    server.send(boundSessionReleased(nonce, c.wire));
    for (int i = 0; i < 100 && !fleet.moveOrdered(); ++i) fleet.pump(20);
    // At once, before any poll(): a refresh is due and the session carries nobody.
    CHECK(fleet.moveOrdered());
    CHECK(fleet.refreshDue());
    CHECK(!fleet.carries(a) && !fleet.carries(b));
    CHECK_EQ(fleet.carriedCount(), 0u);
    CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(a, {{1, 2, 3}, uuid('a'), Bytes(pose, sizeof(pose))}).error()),
             static_cast<int>(Errc::NotCarried));
    CHECK(fleet.player(a)->pending && fleet.player(b)->pending);  // to be carried again
    CHECK_EQ(fleet.players().size(), 3u);
    CHECK(ended.empty());
    // The handler hears the reason from poll(), once. No carry is reported ended one by one.
    fleet.poll();
    CHECK_EQ(ended.size(), 1u);
    CHECK_EQ(static_cast<int>(ended[0].reason), static_cast<int>(c.reason));
    CHECK_EQ(ended[0].wireReason, c.wire);
    CHECK_EQ(std::string(fleetSessionReleaseReasonName(ended[0].reason)), std::string(c.name));
    CHECK(seen.released.empty());
    CHECK_EQ(seen.results.size(), resultsBefore);
    CHECK_EQ(fleet.stats().sessionReleases, 1u);
    // It keeps its socket and its state: what it needs is the game server's refresh.
    CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
    CHECK_EQ(fleet.stats().reconnects, 0u);
    CHECK_EQ(fleet.sessionEpoch(), 1u);

    // No FLEET_CARRY leaves for a server that ended the session, the one that waited and a
    // new one included, and none times out meanwhile.
    CHECK(fleet.carry({tokenFor(later), later}).ok());
    for (int second = 0; second < 8; ++second) {
      clock.advance(1000);
      fleet.pump(0);
      fleet.poll();
    }
    server.expectNothing();
    CHECK_EQ(seen.results.size(), resultsBefore);
    CHECK_EQ(fleet.players().size(), 4u);

    // The refresh answered (the reasons 1, 3 and 4, or a session the API still knows): the
    // capabilities, then every player again, each with a new proof.
    CHECK(fleet.applyRefresh(TokenInfo{kFleetToken, kFleetId, 0}, Assignment{"127.0.0.1", "", server.port}).ok());
    CHECK(!fleet.moveOrdered());
    CHECK(!fleet.refreshDue());
    fleet.pump(0);
    const auto sent = sentSoFar(server);
    CHECK_EQ(sent.size(), 5u);
    CHECK_EQ(sent[0][0], 29u);
    std::vector<std::int64_t> carried;
    for (std::size_t i = 1; i < sent.size(); ++i) {
      CHECK_EQ(sent[i][0], 49u);
      const std::int64_t id = le::readI64(sent[i].data() + wire::offsets::kPayload);
      carried.push_back(readCarry(sent[i], tokenFor(id)).player);
    }
    std::sort(carried.begin(), carried.end());
    CHECK((carried == std::vector<std::int64_t>{a, b, waiting, later}));
    CHECK(!fleet.carries(a));
    for (const std::int64_t id : carried) server.send(own(carryResultFrame(id, 8000 + id, 0)));
    pumpUntil(fleet, [&] { return fleet.carriedCount() == 4; });
    CHECK_EQ(fleet.carriedCount(), 4u);
    CHECK_EQ(seen.results.size(), resultsBefore + 4);
    CHECK(fleet.sendActorUpdate(a, {{1, 2, 3}, uuid('a'), Bytes(pose, sizeof(pose))}).ok());
    CHECK_EQ(ended.size(), 1u);
  }
  CHECK_EQ(static_cast<int>(FleetSessionReleaseReason::Deauthorized), 0);
  CHECK_EQ(static_cast<int>(FleetSessionReleaseReason::TokenExpired), 2);
}

// A frame that is not the server's word changes nothing: one whose HMAC does not hold, one
// signed with another key, one bound to another connection (a frame captured earlier), and
// the unbound form (28) once the server has acknowledged the bound forms. The unbound form
// counts before that acknowledgement, as it does for COMMAND_RECONNECT. The idle reason
// keeps its own meaning. With FleetConfig::reassign the session asks the game server.
void runOnlyAVerifiedReleaseEndsTheSession() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  std::vector<FleetSessionReleased> ended;
  FleetHandlers h;
  h.sessionReleased = [&](const FleetSessionReleased& r) { ended.push_back(r); };
  fleet.setHandlers(std::move(h));
  server.serves(fleet);
  CHECK(fleet.open().ok());
  fleet.pump(0);
  const auto first = sentSoFar(server);
  CHECK_EQ(first.size(), 1u);
  const std::vector<std::uint8_t> nonce(first[0].begin() + wire::offsets::kPayload + 4,
                                        first[0].begin() + wire::offsets::kPayload + 12);
  const std::int64_t a = 821;
  carryAs(fleet, server, a, 8021, tokenFor(a));
  const auto unchanged = [&] {
    settle(fleet);
    CHECK(!fleet.moveOrdered());
    CHECK(!fleet.refreshDue());
    CHECK(fleet.carries(a));
    CHECK(ended.empty());
    CHECK_EQ(fleet.stats().sessionReleases, 0u);
    CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
  };

  // Its HMAC does not hold.
  auto forged = boundSessionReleased(nonce, 0);
  forged.back() ^= 0x01;
  server.send(forged);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  unchanged();
  // Signed by somebody who holds a player's token.
  server.send(boundSessionReleased(nonce, 0, kPlayerToken));
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 2; });
  unchanged();
  // The reason rewritten in flight.
  auto rewritten = boundSessionReleased(nonce, 5);
  rewritten[1] = 0;
  server.send(rewritten);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 3; });
  unchanged();
  // Bound to another connection: a frame captured earlier and sent again.
  std::vector<std::uint8_t> other = nonce;
  other[0] ^= 0xff;
  server.send(boundSessionReleased(other, 0));
  pumpUntil(fleet, [&] { return fleet.stats().staleBoundControl == 1; });
  unchanged();
  // The server acknowledged the bound forms: an unbound 28, which signs two fixed bytes and
  // so verifies for as long as the token lives, is one captured earlier.
  server.send(acknowledgementOf(first[0]));
  pumpUntil(fleet, [&] { return fleet.stats().boundControlAcks == 1; });
  server.send(legacySessionReleased(0));
  pumpUntil(fleet, [&] { return fleet.stats().legacyControlDropped == 1; });
  unchanged();
  CHECK_EQ(fleet.stats().hmacFailures, 3u);

  // The bound frame that verifies does end it.
  server.send(boundSessionReleased(nonce, 0));
  pumpUntil(fleet, [&] { return !ended.empty(); });
  CHECK_EQ(ended.size(), 1u);
  CHECK(fleet.moveOrdered());
  CHECK(!fleet.carries(a));
  // Sent again (it is sent once; a copy changes nothing more).
  server.send(boundSessionReleased(nonce, 0));
  pumpUntil(fleet, [&] { return ended.size() == 2; });
  CHECK(fleet.moveOrdered());
  CHECK_EQ(fleet.players().size(), 1u);

  // Before any acknowledgement the unbound form is the server's word too.
  {
    FakeServer plain;
    plain.start();
    FleetSession session(configFor(plain), crypto(), clock);
    std::vector<FleetSessionReleased> heard;
    FleetHandlers handlers;
    handlers.sessionReleased = [&](const FleetSessionReleased& r) { heard.push_back(r); };
    session.setHandlers(std::move(handlers));
    openConnected(session, plain);
    carryAs(session, plain, a, 8021, tokenFor(a));
    plain.send(legacySessionReleased(2));
    pumpUntil(session, [&] { return !heard.empty(); });
    CHECK_EQ(heard.size(), 1u);
    CHECK_EQ(static_cast<int>(heard[0].reason), static_cast<int>(FleetSessionReleaseReason::TokenExpired));
    CHECK(session.refreshDue());
    CHECK(!session.carries(a));
  }

  // The idle reason, bound: the session ends as a player's does (Inactive), and this
  // handler is not for it.
  {
    FakeServer idle;
    idle.start();
    FleetSession session(configFor(idle), crypto(), clock);
    std::vector<FleetSessionReleased> heard;
    FleetHandlers handlers;
    handlers.sessionReleased = [&](const FleetSessionReleased& r) { heard.push_back(r); };
    session.setHandlers(std::move(handlers));
    idle.serves(session);
    CHECK(session.open().ok());
    session.pump(0);
    const auto bound = capabilitiesNonce(idle);
    idle.send(boundSessionReleased(bound, 5));
    pumpUntil(session, [&] { return session.state() == ConnState::Inactive; });
    CHECK_EQ(static_cast<int>(session.state()), static_cast<int>(ConnState::Inactive));
    CHECK(heard.empty());
    CHECK(!session.moveOrdered());
    CHECK_EQ(session.stats().sessionReleases, 0u);
  }

  // With FleetConfig::reassign the game server is asked on the network thread: a server it
  // answers is where the session carries its players again; an error (the refresh said
  // NOT_FOUND) fails the session, which the game server then replaces.
  for (const bool found : {true, false}) {
    FakeServer asked;
    asked.start();
    FleetConfig cfg = configFor(asked);
    int calls = 0;
    cfg.reassign = [&]() -> Result<Assignment> {
      ++calls;
      if (!found) return Errc::Rejected;
      return Assignment{"127.0.0.1", "", asked.port};
    };
    FleetSession session(cfg, crypto(), clock);
    asked.serves(session);
    CHECK(session.open().ok());
    session.pump(0);
    const auto bound = capabilitiesNonce(asked);
    carryAs(session, asked, a, 8021, tokenFor(a));
    asked.send(boundSessionReleased(bound, 0));
    pumpUntil(session, [&] { return calls == 1 && session.state() != ConnState::Reconnecting; });
    CHECK_EQ(calls, 1);
    CHECK(!session.carries(a));
    if (!found) {
      CHECK_EQ(static_cast<int>(session.state()), static_cast<int>(ConnState::Failed));
      continue;
    }
    pumpUntil(session, [&] { return session.state() == ConnState::Connected; });
    session.pump(0);
    CHECK(!session.moveOrdered());
    const auto sent = sentSoFar(asked);
    CHECK_EQ(sent.size(), 2u);
    CHECK_EQ(sent[0][0], 29u);
    CHECK_EQ(readCarry(sent[1], tokenFor(a)).player, a);
  }
}

// The one datagram that says the session ended can be lost. Then nothing verified arrives
// (a forged frame does not count), and the silence timeout makes the refresh due 45 seconds
// later, as before. If the frame arrives after all, the session takes it.
void runALostReleaseLeavesTheSilenceTimeout() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(watchedConfigFor(server), crypto(), clock);
  std::vector<FleetSessionReleased> ended;
  FleetHandlers h;
  h.sessionReleased = [&](const FleetSessionReleased& r) { ended.push_back(r); };
  fleet.setHandlers(std::move(h));
  server.serves(fleet);
  CHECK(fleet.open().ok());
  fleet.pump(0);
  const auto nonce = capabilitiesNonce(server);
  const std::int64_t a = 831;
  carryAs(fleet, server, a, 8031, tokenFor(a));  // the last thing the server says
  auto forged = boundSessionReleased(nonce, 0);
  forged.back() ^= 0x01;
  int dueAfter = -1;
  for (int second = 1; second <= 60 && dueAfter < 0; ++second) {
    clock.advance(1000);
    if (second % 10 == 0) server.send(forged);
    fleet.pump(0);
    fleet.pump(20);
    fleet.poll();
    if (fleet.refreshDue()) dueAfter = second;
  }
  CHECK(dueAfter == 46 || dueAfter == 47);
  CHECK(fleet.moveOrdered());
  CHECK(ended.empty());
  CHECK(fleet.carries(a));  // the silence alone ends no carry here
  CHECK_EQ(fleet.stats().sessionReleases, 0u);
  // The frame, late: the carries are over, and the refresh is still the one that is due.
  server.send(boundSessionReleased(nonce, 0));
  pumpUntil(fleet, [&] { return !ended.empty(); });
  CHECK_EQ(ended.size(), 1u);
  CHECK(!fleet.carries(a));
  CHECK(fleet.moveOrdered());
}

// ---------------------------------------------------------------------------
// setToken: a refreshed fleet token (the same token and id, a new expiry) changes nothing
// on the wire; another id is another session, whose capabilities and carries follow.
// ---------------------------------------------------------------------------

void runSetToken() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  openConnected(fleet, server);
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);

  CHECK_EQ(fleet.token().expiresAtEpochMs, 0);
  CHECK(fleet.setToken(TokenInfo{kFleetToken, kFleetId, kEpochMs + 86'400'000}).ok());
  CHECK_EQ(fleet.token().expiresAtEpochMs, kEpochMs + 86'400'000);
  CHECK_EQ(fleet.token().gameTokenId, kFleetId);
  fleet.pump(0);
  server.expectNothing();
  CHECK(fleet.carries(kPlayerId));
  CHECK(!fleet.player(kPlayerId)->pending);
  CHECK_EQ(static_cast<int>(fleet.setToken(TokenInfo{"short", kFleetId, 0}).code),
           static_cast<int>(Errc::InvalidArgument));

  // Another fleet token and id (a session opened again): its capabilities under the new id,
  // then the carry, whose proof names the new id and which the new token signs.
  const std::string nextToken(64, 'n');
  CHECK(fleet.setToken(TokenInfo{nextToken, 9002, 0}).ok());
  CHECK_EQ(expectCapabilities(server, 9002, nextToken), 0x03u);
  auto got = server.recvOne();
  CHECK_EQ(readCarry(got, kPlayerToken, 9002, nextToken).player, kPlayerId);
  CHECK(!fleet.carries(kPlayerId));
  CHECK(fleet.player(kPlayerId)->pending);
  // Its result arrives signed with the new token.
  server.send(own(carryResultFrame(kPlayerId, 77, 0), *wire::Token64::fromString(nextToken)));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  CHECK(fleet.carries(kPlayerId));
}

// ---------------------------------------------------------------------------
// An error frame ([3][sequence][code]) carries no HMAC of its own. It answers one send, and
// changes nothing in a fleet session whatever its code and however it arrived: no carry
// ends on it and none makes a refresh due, so nobody who can put three bytes on the
// session's flow can make the game server call refreshFleetSession. (TOKEN_EXPIRED is about
// a carried player's token; the fleet token's own end is a signed COMMAND_SESSION_RELEASED.)
// Inside a signed bundle that verifies it is handed over marked verified.
// ---------------------------------------------------------------------------

void runErrorFramesChangeNothing() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);  // no FleetConfig::reassign
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);

  const std::vector<std::uint8_t> expired = {3, 9, 32};       // TOKEN_EXPIRED for sequence 9
  const std::vector<std::uint8_t> unauthorized = {3, 10, 7};  // UNAUTHORIZED for sequence 10

  // Alone, and inside a plain bundle: nothing vouches for them. A hundred of them.
  for (int i = 0; i < 50; ++i) {
    server.send(expired);
    server.send(plainBundle({expired, unauthorized}));
  }
  pumpUntil(fleet, [&] { return seen.errors.size() == 150; });
  settle(fleet);
  CHECK_EQ(seen.errors.size(), 150u);
  for (const GenericError& e : seen.errors) CHECK(!e.verified);
  CHECK_EQ(static_cast<int>(seen.errors[0].code), 32);
  CHECK_EQ(seen.errors[0].sequence, 9u);
  // No state moved: no move order, no refresh due, no reassignment, the carries as they were.
  CHECK(!fleet.moveOrdered());
  CHECK(!fleet.refreshDue());
  CHECK_EQ(fleet.sessionEpoch(), 1u);
  CHECK_EQ(fleet.stats().reconnects, 0u);
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
  CHECK(fleet.carries(kPlayerId));
  server.expectNothing();

  // A signed bundle whose HMAC does not hold is dropped whole: its error reaches nobody.
  auto forged = signedBundle({expired});
  forged.back() ^= 0x01;
  server.send(forged);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  settle(fleet);
  CHECK_EQ(seen.errors.size(), 150u);
  CHECK(!fleet.moveOrdered());

  // Inside a signed bundle that verifies, each is verified, and still only information:
  // an UNAUTHORIZED, and a TOKEN_EXPIRED too.
  server.send(signedBundle({unauthorized}));
  pumpUntil(fleet, [&] { return seen.errors.size() == 151; });
  CHECK(seen.errors.back().verified);
  CHECK_EQ(static_cast<int>(seen.errors.back().code), 7);
  CHECK(!fleet.moveOrdered());
  CHECK_EQ(fleet.sessionEpoch(), 1u);

  server.send(signedBundle({expired}));
  pumpUntil(fleet, [&] { return seen.errors.size() == 152; });
  settle(fleet);
  CHECK_EQ(seen.errors.size(), 152u);
  CHECK(seen.errors.back().verified);
  CHECK_EQ(static_cast<int>(seen.errors.back().code), 32);
  CHECK(!fleet.moveOrdered());
  CHECK(!fleet.refreshDue());
  CHECK_EQ(fleet.sessionEpoch(), 1u);
  CHECK_EQ(fleet.stats().reconnects, 0u);
  CHECK(fleet.carries(kPlayerId));
  server.expectNothing();

  // A player's Connection is as it was: it acts on TOKEN_EXPIRED however it arrives, and
  // its handler is told which way that was.
  struct Counting final : ISessionProvider {
    int port;
    int assigns = 0;
    explicit Counting(int p) : port(p) {}
    Result<Assignment> assignServer() override {
      ++assigns;
      return Assignment{"127.0.0.1", "", port};
    }
    Result<TokenInfo> refreshToken() override { return Errc::Rejected; }
  };
  FakeServer other;
  other.start();
  Config plain;
  plain.appId = kAppId;
  plain.token = TokenInfo{kFleetToken, kFleetId, 0};
  plain.manualPump = true;
  plain.sessionReadyWaitMs = 0;
  plain.bundleSends = false;
  plain.advertiseCapabilities = false;
  auto provider = std::make_shared<Counting>(other.port);
  Connection conn(plain, provider, crypto());
  std::vector<GenericError> errors;
  Handlers handlers;
  handlers.genericError = [&](const GenericError& e) { errors.push_back(e); };
  conn.setHandlers(std::move(handlers));
  CHECK(conn.connect().ok());
  conn.pump(0);
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  other.recvOne();
  CHECK_EQ(provider->assigns, 1);
  other.send(expired);
  for (int i = 0; i < 100 && provider->assigns < 2; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(provider->assigns, 2);  // unverified, and acted on, as before
  CHECK_EQ(errors.size(), 1u);
  CHECK(!errors[0].verified);
  conn.pump(0);
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  other.recvOne();
  other.send(signedBundle({unauthorized}));
  for (int i = 0; i < 100 && errors.size() < 2; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(errors.size(), 2u);
  CHECK(errors[1].verified);
}

// ---------------------------------------------------------------------------
// A status 3 (a replay) can answer an earlier send that reached the server after a later
// one was accepted, and a result does not say which send it answers. While a carry that
// left has no answer, a status 3 settles nothing: at the first, one more carry leaves. The
// carry ends ProofRefused when every carry that left was refused, or at its deadline.
// ---------------------------------------------------------------------------

void runProofRefusedConverges() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);

  // Two carries leave, A at 0 ms and B at 250 ms.
  const auto twoSends = [&](std::vector<FleetCarryResult>& viaCallback) {
    CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                  viaCallback.push_back(r);
                }).ok());
    const Carry a = readCarry(server.recvOne(), kPlayerToken);
    clock.advance(250);
    fleet.pump(0);
    const Carry b = readCarry(server.recvOne(), kPlayerToken);
    CHECK(b.epochMs == a.epochMs + 250);
    return std::vector<Carry>{a, b};
  };

  // The reordered answers. The server accepted B and its 51 was lost; A arrived after B, so
  // it is a replay, and its 51 (status 3) is the only answer the session hears.
  {
    std::vector<FleetCarryResult> viaCallback;
    const auto sent = twoSends(viaCallback);
    clock.advance(10);
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    pumpUntil(fleet, [&] { return fleet.stats().messagesSentByType[49] == 3; });
    // Not settled: nothing reported, the row still waits, and one more carry left at once
    // with a new time and proof.
    CHECK(viaCallback.empty());
    CHECK(seen.results.empty());
    CHECK(fleet.player(kPlayerId).has_value());
    CHECK(fleet.player(kPlayerId)->pending);
    const Carry c = readCarry(server.recvOne(), kPlayerToken);
    CHECK_EQ(c.epochMs, sent[1].epochMs + 10);
    CHECK(c.proof != sent[0].proof && c.proof != sent[1].proof);
    // The server carries the player (from B), and answers this newer carry: carried.
    server.send(own(carryResultFrame(kPlayerId, 77, 0)));
    pumpUntil(fleet, [&] { return !viaCallback.empty(); });
    CHECK_EQ(viaCallback.size(), 1u);
    CHECK(viaCallback[0].carried());
    CHECK_EQ(viaCallback[0].userId, 77);
    CHECK_EQ(seen.results.size(), 1u);
    CHECK(fleet.carries(kPlayerId));
    // A's late status 3, were it to arrive now, is an answer nobody waits for.
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    settle(fleet);
    CHECK(fleet.carries(kPlayerId));
    CHECK_EQ(seen.results.size(), 1u);
    CHECK(fleet.release(kPlayerId).ok());
    CHECK_EQ(server.recvOne()[0], 50u);
  }

  // Two earlier carries arrive late. The server accepted the third carry, C, and its 51 was
  // lost; A and B reached it after C, so each is answered 3. The second status 3 arrives
  // after the follow-up carry left, and still settles nothing: that carry has no answer yet.
  {
    std::vector<FleetCarryResult> viaCallback;
    twoSends(viaCallback);
    clock.advance(250);
    fleet.pump(0);
    readCarry(server.recvOne(), kPlayerToken);  // C
    clock.advance(10);
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));  // A's
    readCarry(nextOfTypeAfterPump(fleet, server), kPlayerToken);  // the follow-up, D
    const auto heard = fleet.stats().messagesReceivedByType[51];
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));  // B's
    pumpUntil(fleet, [&] { return fleet.stats().messagesReceivedByType[51] == heard + 1; });
    settle(fleet);
    CHECK(viaCallback.empty());
    CHECK(fleet.player(kPlayerId)->pending);
    server.expectNothing();  // one follow-up, at the first status 3 only
    server.send(own(carryResultFrame(kPlayerId, 77, 0)));  // D's: the server carries the player
    pumpUntil(fleet, [&] { return !viaCallback.empty(); });
    CHECK_EQ(viaCallback.size(), 1u);
    CHECK(viaCallback[0].carried());
    CHECK(fleet.carries(kPlayerId));
    CHECK(fleet.release(kPlayerId).ok());
    CHECK_EQ(server.recvOne()[0], 50u);
  }

  // A proof the server does refuse: every carry that left is answered 3, the follow-up
  // too, and the last of those answers settles it, once.
  {
    std::vector<FleetCarryResult> viaCallback;
    const std::size_t before = seen.results.size();
    const auto sent = twoSends(viaCallback);
    // The first refusal arrives in the millisecond the second carry left. The follow-up is
    // dated the next millisecond: the same time would make it the same bytes, a replay.
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    const Carry follow = readCarry(nextOfTypeAfterPump(fleet, server), kPlayerToken);
    CHECK_EQ(follow.epochMs, sent[1].epochMs + 1);
    CHECK(follow.proof != sent[1].proof);
    CHECK(viaCallback.empty());
    const auto heard = fleet.stats().messagesReceivedByType[51];
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    pumpUntil(fleet, [&] { return fleet.stats().messagesReceivedByType[51] == heard + 1; });
    settle(fleet);
    CHECK(viaCallback.empty());  // two refusals, three carries
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    pumpUntil(fleet, [&] { return !viaCallback.empty(); });
    CHECK_EQ(viaCallback.size(), 1u);
    CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::ProofRefused));
    CHECK_EQ(viaCallback[0].wireStatus, 3u);
    CHECK(!fleet.player(kPlayerId).has_value());
    CHECK_EQ(seen.results.size(), before + 1);
    clock.advance(1000);
    fleet.pump(0);
    fleet.poll();
    server.expectNothing();
    CHECK_EQ(viaCallback.size(), 1u);
  }

  // After the status 3, no answer at all: the attempts run out, and the carry ends as the
  // last answer it had, ProofRefused, where one never answered ends TimedOut.
  {
    std::vector<FleetCarryResult> viaCallback;
    twoSends(viaCallback);  // the first send was 250 ms ago
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    readCarry(nextOfTypeAfterPump(fleet, server), kPlayerToken);
    for (int step = 0; step < 18; ++step) {
      clock.advance(250);
      fleet.pump(0);
      fleet.poll();
      drain(server);
      CHECK(viaCallback.empty());
    }
    clock.advance(250);  // 5,000 ms after the first send
    fleet.pump(0);
    fleet.poll();
    CHECK_EQ(viaCallback.size(), 1u);
    CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::ProofRefused));
    CHECK_EQ(viaCallback[0].wireStatus, 3u);
    CHECK(!fleet.player(kPlayerId).has_value());
    drain(server);
  }

  // A status 3 to a carry's only send is that send's answer: settled on the spot.
  {
    std::vector<FleetCarryResult> viaCallback;
    CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                  viaCallback.push_back(r);
                }).ok());
    readCarry(server.recvOne(), kPlayerToken);
    server.send(own(carryResultFrame(kPlayerId, 0, 3)));
    pumpUntil(fleet, [&] { return !viaCallback.empty(); });
    CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::ProofRefused));
    server.expectNothing();
  }

  // Both of two carries are answered 3 before the follow-up could leave (the rate had no
  // turn for it): every carry that left was refused, so that settles it, and the follow-up
  // never leaves.
  {
    FakeServer slow;
    slow.start();
    FleetConfig paced = configFor(slow);
    paced.carryBurst = 2;
    paced.carriesPerSecond = 1;
    FleetSession waiting(paced, crypto(), clock);
    std::vector<FleetCarryResult> results;
    FleetHandlers h;
    h.carryResult = [&](const FleetCarryResult& r) { results.push_back(r); };
    waiting.setHandlers(std::move(h));
    openConnected(waiting, slow);
    CHECK(waiting.carry({kPlayerToken, kPlayerId}).ok());
    readCarry(slow.recvOne(), kPlayerToken);
    clock.advance(500);
    waiting.pump(0);  // the second of the burst's two turns (half a turn has come back since)
    readCarry(slow.recvOne(), kPlayerToken);
    slow.send(own(carryResultFrame(kPlayerId, 0, 3)));
    pumpUntil(waiting, [&] { return waiting.stats().messagesReceivedByType[51] == 1; });
    settle(waiting);
    CHECK(results.empty());  // one refusal, two carries
    CHECK(waiting.player(kPlayerId)->pending);
    slow.expectNothing();  // no turn yet for the follow-up
    slow.send(own(carryResultFrame(kPlayerId, 0, 3)));
    pumpUntil(waiting, [&] { return !results.empty(); });
    CHECK_EQ(results.size(), 1u);
    CHECK_EQ(static_cast<int>(results[0].status), static_cast<int>(FleetCarryStatus::ProofRefused));
    CHECK(!waiting.player(kPlayerId).has_value());
    clock.advance(2000);
    waiting.pump(0);
    waiting.poll();
    slow.expectNothing();
  }
}

// ---------------------------------------------------------------------------
// A send the socket refuses for a real fault is not a full buffer: it comes back
// SocketError, is counted in sendsFailed, and the log names the system's error code. The
// fault here is the one a closed port gives a connected UDP socket (ECONNREFUSED on the
// send after the refusal came back).
// ---------------------------------------------------------------------------

void runSendFaultIsNamed() {
  RecordingLogger logger;
  ManualClock clock;
  auto server = std::make_unique<FakeServer>();
  server->start();
  FleetSession fleet(configFor(*server), crypto(), clock, logger);
  openConnected(fleet, *server);
  CHECK_EQ(fleet.stats().sendsFailed, 0u);
  server.reset();  // the port is closed: the next datagram is refused, and the send after it fails

  Errc seen = Errc::Ok;
  for (int i = 0; i < 400 && seen == Errc::Ok; ++i) {
    const auto st = fleet.sendHeartbeat();
    if (!st.ok()) seen = st.error();
    else std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(seen, Errc::SocketError);  // a fault, and not WouldBlock: sending again would hide it
  const auto stats = fleet.stats();
  CHECK_EQ(stats.sendsFailed, 1u);
  CHECK_EQ(stats.sendsDeferred, 0u);
  std::lock_guard lock(logger.mutex);
  const std::string expected = "system error " + std::to_string(ECONNREFUSED) + " (1 so far)";
  bool named = false;
  for (const auto& line : logger.lines) {
    if (line.find("SocketError") != std::string::npos && line.find(expected) != std::string::npos)
      named = true;
  }
  if (!named) {
    for (const auto& line : logger.lines) std::fprintf(stderr, "logged: %s\n", line.c_str());
  }
  CHECK(named);
}

// ---------------------------------------------------------------------------
// A fleet session is in one app. A notification that names another app is not the session's,
// however it was signed: it is dropped, counted in Stats::wrongApp and reaches no handler,
// plain or inside a FLEET_DELIVERY. (The realtime servers should never send one; a live run
// against the build before v0.50.1 saw it happen.) A plain notification nothing signed is
// dropped and counted
// in Stats::unsignedNotifications.
// ---------------------------------------------------------------------------

void runOnlyTheSessionsOwnAppIsDispatched() {
  constexpr std::int64_t kOtherApp = kAppId + 1;
  FakeServer server;
  server.start();
  ManualClock clock;
  RecordingLogger logger;
  FleetSession fleet(configFor(server), crypto(), clock, logger);
  struct Got {
    std::int64_t appId;
    std::string payload;
    bool forSession;
  };
  std::vector<Got> got;
  int any = 0, channel = 0;
  FleetHandlers h;
  const auto record = [&](const SpatialNotification& n, FleetRecipients r) {
    got.push_back({n.appId, std::string(n.payload.begin(), n.payload.end()), r.forSession()});
  };
  h.actorUpdate = record;
  h.text = record;
  h.genericSpatial = record;
  h.any = [&](const SpatialNotification&, FleetRecipients) { ++any; };
  h.channelMessage = [&](const ChannelNotification&, FleetRecipients) { ++channel; };
  fleet.setHandlers(std::move(h));
  openConnected(fleet, server);
  const std::int64_t a = 721;
  carryAs(fleet, server, a, 7021, tokenFor(a));
  const auto note = [](const char* text) { return Bytes(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)); };
  const auto arrived = [&](std::size_t dispatched, std::uint64_t wrongApp, std::uint64_t notSigned) {
    pumpUntil(fleet, [&] {
      const auto s = fleet.stats();
      return got.size() == dispatched && s.wrongApp == wrongApp && s.unsignedNotifications == notSigned;
    });
    settle(fleet);
    const auto s = fleet.stats();
    CHECK_EQ(got.size(), dispatched);
    CHECK_EQ(s.wrongApp, wrongApp);
    CHECK_EQ(s.unsignedNotifications, notSigned);
    CHECK_EQ(static_cast<std::size_t>(any), dispatched);
  };

  // Plain, in a signed bundle that verifies: the session's own state between two of another
  // app's. Only its own is dispatched.
  server.send(signedBundle(
      {unsignedNote(wire::MessageType::ActorUpdateNotification, note("theirs 1"), 1, kOtherApp),
       unsignedNote(wire::MessageType::ActorUpdateNotification, note("mine 1"), 2),
       unsignedNote(wire::MessageType::GenericSpatial1, note("theirs 2"), 3, kOtherApp)}));
  arrived(1, 2, 0);
  CHECK_EQ(got[0].payload, std::string("mine 1"));
  CHECK_EQ(got[0].appId, kAppId);

  // Plain and alone, with its own HMAC, which holds: another app's all the same.
  server.send(notification(wire::MessageType::ActorUpdateNotification, note("theirs 3"), 4, fleetKey(), kOtherApp));
  arrived(1, 3, 0);
  server.send(notification(wire::MessageType::ActorUpdateNotification, note("mine 2"), 5, fleetKey()));
  arrived(2, 3, 0);

  // Inside a FLEET_DELIVERY, with the frame's own HMAC and inside a signed bundle: the
  // delivery lists the session's player, and the text is another app's.
  server.send(own(deliveryFrame({a}, unsignedNote(wire::MessageType::ClientTextNotification, note("theirs 4"), 6, kOtherApp))));
  arrived(2, 4, 0);
  server.send(signedBundle(
      {deliveryFrame({a}, unsignedNote(wire::MessageType::ClientTextNotification, note("theirs 5"), 7, kOtherApp)),
       deliveryFrame({a}, unsignedNote(wire::MessageType::ClientTextNotification, note("mine 3"), 8))}));
  arrived(3, 5, 0);
  CHECK_EQ(got[2].payload, std::string("mine 3"));
  CHECK(!got[2].forSession);

  // A plain notification nothing signed (no HMAC of its own, outside a signed bundle), alone
  // and in a plain bundle: dropped, the session's own app or another's.
  server.send(unsignedNote(wire::MessageType::ActorUpdateNotification, note("unsigned 1"), 9));
  arrived(3, 5, 1);
  server.send(plainBundle({unsignedNote(wire::MessageType::ActorUpdateNotification, note("unsigned 2"), 10),
                           unsignedNote(wire::MessageType::ActorUpdateNotification, note("unsigned 3"), 11, kOtherApp),
                           notification(wire::MessageType::ActorUpdateNotification, note("mine 4"), 12, fleetKey())}));
  arrived(4, 5, 3);
  CHECK_EQ(got[3].payload, std::string("mine 4"));

  // Nothing of another app reached a handler, and none of these counts as a bad HMAC or as
  // unreadable: the frames were what the server signed.
  for (const auto& g : got) CHECK_EQ(g.appId, kAppId);
  const auto stats = fleet.stats();
  CHECK_EQ(stats.hmacFailures, 0u);
  CHECK_EQ(stats.malformed, 0u);
  // A channel notification names no app: it is as it was.
  const std::uint8_t body[] = {'h', 'i'};
  server.send(signedBundle({deliveryFrame({a}, channelNote(18, 55, Bytes(body, 2)))}));
  pumpUntil(fleet, [&] { return channel == 1; });
  CHECK_EQ(channel, 1);

  // The log names what was dropped, at the first and at every doubling of the count: the
  // app it named, the message type and the chunk.
  std::lock_guard lock(logger.mutex);
  int ofAnotherApp = 0, notSigned = 0;
  for (const auto& line : logger.lines) {
    if (line.find("dropped a notification of another app: app " + std::to_string(kOtherApp) +
                  ", message type 130, chunk (1, 2, 3), 1 so far") != std::string::npos)
      ++ofAnotherApp;
    if (line.find("of another app") != std::string::npos && line.find("4 so far") != std::string::npos) ++ofAnotherApp;
    if (line.find("dropped a notification nothing signed: app " + std::to_string(kAppId)) != std::string::npos) ++notSigned;
    CHECK(line.find("report it to Crowded Kingdoms") != std::string::npos);
  }
  CHECK_EQ(ofAnotherApp, 2);
  CHECK_EQ(notSigned, 2);  // the first and the second
  CHECK_EQ(logger.lines.size(), 5u);  // wrong app at 1, 2 and 4; unsigned at 1 and 2
}

// ---------------------------------------------------------------------------
// Realtime servers v0.50.1 send a fleet session its refusals inside its signed bundles, so a
// refusal is verified. The golden bundles are wire_test's (the script is quoted there): the
// three bytes [3][sequence][code] and the bundle's HMAC over them, keyed with the fleet token.
// A verified refusal still changes no carry. When the sequence number it echoes can only be
// one carried player's send, FleetHandlers::sendRefused names that player.
// ---------------------------------------------------------------------------

// MESSAGE_BUNDLE_SIGNED around [3][42][7]: UNAUTHORIZED for sequence 42.
const char kGoldenSignedBundleRefusal[] =
    "1e0300032a077cbee1f7148389389ea17c7838dca341e38abcb4ba132b565587681f7421c8cc";
// MESSAGE_BUNDLE_SIGNED around [3][43][32] and [3][44][7]: TOKEN_EXPIRED 43, UNAUTHORIZED 44.
const char kGoldenSignedBundleTwoRefusals[] =
    "1e0300032b200300032c07a5f9c60c0cce601e55629c62fb6d5a0d65c6f8bd0a3a1f23e8fcd45ff46a660d";

void runVerifiedRefusals() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  std::vector<FleetSendRefused> refused;
  FleetHandlers h = recording(seen);
  h.sendRefused = [&](const FleetSendRefused& r) { refused.push_back(r); };
  fleet.setHandlers(std::move(h));
  openConnected(fleet, server);
  const std::int64_t a = 901, b = 902;
  carryAs(fleet, server, a, 9011, tokenFor(a));
  carryAs(fleet, server, b, 9012, tokenFor(b));
  const std::uint8_t pose[] = {1, 2, 3};
  const SpatialSend update{{1, 2, 3}, uuid('a'), Bytes(pose, sizeof(pose))};
  // The sequence number the session's last message took: the next takes the one after.
  std::uint8_t last = fleet.sendActorUpdate(a, update).value();
  // Send for `filler` until the next number is `sequence`, then for `player` under it.
  const auto sendAs = [&](std::int64_t player, std::uint8_t sequence, std::int64_t filler) {
    while (static_cast<std::uint8_t>(last + 1) != sequence) {
      const auto sent = fleet.sendActorUpdate(filler, update);
      CHECK(sent.ok());
      last = sent.value();
    }
    const auto sent = fleet.sendActorUpdate(player, update);
    CHECK(sent.ok());
    CHECK_EQ(sent.value(), sequence);
    last = sent.value();
    (void)sentSoFar(server);
  };

  // The golden bundle: verified, handed to genericError, and the send it answers was a's.
  sendAs(a, 42, a);
  server.send(fromHex(kGoldenSignedBundleRefusal));
  pumpUntil(fleet, [&] { return !seen.errors.empty(); });
  CHECK_EQ(seen.errors.size(), 1u);
  CHECK(seen.errors[0].verified);
  CHECK_EQ(seen.errors[0].sequence, 42u);
  CHECK_EQ(static_cast<int>(seen.errors[0].code), static_cast<int>(wire::ErrorCode::Unauthorized));
  CHECK_EQ(refused.size(), 1u);
  CHECK_EQ(refused[0].playerGameTokenId, a);
  CHECK_EQ(refused[0].userId, 9011);
  CHECK_EQ(refused[0].sequence, 42u);
  CHECK_EQ(static_cast<int>(refused[0].code), static_cast<int>(wire::ErrorCode::Unauthorized));
  CHECK(!refused[0].playerTokenExpired());
  CHECK_EQ(fleet.stats().signedBundlesReceived, 1u);
  CHECK_EQ(fleet.stats().hmacFailures, 0u);
  // It changed nothing: the carry stands, and no refresh is due.
  CHECK(fleet.carries(a));
  CHECK(!fleet.moveOrdered() && !fleet.refreshDue());
  CHECK(seen.released.empty());

  // Two refusals in one bundle: TOKEN_EXPIRED for b's send (the player's own token expired:
  // the game server refreshes it and carries again) and UNAUTHORIZED for a's.
  sendAs(b, 43, a);
  sendAs(a, 44, a);
  server.send(fromHex(kGoldenSignedBundleTwoRefusals));
  pumpUntil(fleet, [&] { return seen.errors.size() == 3; });
  CHECK_EQ(seen.errors.size(), 3u);
  CHECK(seen.errors[1].verified && seen.errors[2].verified);
  CHECK_EQ(refused.size(), 3u);
  CHECK_EQ(refused[1].playerGameTokenId, b);
  CHECK_EQ(refused[1].userId, 9012);
  CHECK(refused[1].playerTokenExpired());
  CHECK_EQ(refused[2].playerGameTokenId, a);
  CHECK(fleet.carries(a) && fleet.carries(b));
  CHECK(!fleet.moveOrdered() && !fleet.refreshDue());
  CHECK_EQ(fleet.stats().reconnects, 0u);
  CHECK_EQ(fleet.stats().sendRefusalsNamed, 3u);

  // The same three bytes with nothing signing them are a hint: genericError hears them
  // unverified, and no player is named on their word.
  server.send(std::vector<std::uint8_t>{3, 44, 7});
  server.send(plainBundle({{3, 43, 32}}));
  pumpUntil(fleet, [&] { return seen.errors.size() == 5; });
  CHECK(!seen.errors[3].verified && !seen.errors[4].verified);
  CHECK_EQ(refused.size(), 3u);
  CHECK(fleet.carries(a) && fleet.carries(b));

  // A sequence number is one byte for the whole session. When sends for two players left
  // under the same number within the time a refusal can take, the refusal names neither.
  sendAs(a, 50, a);
  sendAs(b, 50, b);  // 256 sends later, the same number, for b
  server.send(signedBundle({{3, 50, 7}}));
  pumpUntil(fleet, [&] { return seen.errors.size() == 6; });
  CHECK(seen.errors[5].verified);
  CHECK_EQ(refused.size(), 3u);
  // Once the other player's send is too old to be the one refused, the number names b.
  clock.advance(kFleetRefusalWindowMs + 1);
  sendAs(b, 50, b);
  server.send(signedBundle({{3, 50, 7}}));
  pumpUntil(fleet, [&] { return seen.errors.size() == 7; });
  CHECK_EQ(refused.size(), 4u);
  CHECK_EQ(refused[3].playerGameTokenId, b);
  // A refusal that arrives later than a refusal can names nobody.
  clock.advance(kFleetRefusalWindowMs + 1);
  server.send(signedBundle({{3, 50, 7}}));
  pumpUntil(fleet, [&] { return seen.errors.size() == 8; });
  CHECK_EQ(refused.size(), 4u);

  // A message of the session's own (its keepalive) under a number is not a player's.
  for (int i = 0; i < 300; ++i) {
    const auto own = fleet.sendHeartbeat();
    CHECK(own.ok());
    if (own.value() == 60) break;
  }
  (void)sentSoFar(server);
  server.send(signedBundle({{3, 60, 18}}));  // INVALID_APP_ID for a keepalive
  pumpUntil(fleet, [&] { return seen.errors.size() == 9; });
  CHECK(seen.errors[8].verified);
  CHECK_EQ(refused.size(), 4u);
  CHECK_EQ(fleet.stats().sendRefusalsNamed, 4u);
}

// ---------------------------------------------------------------------------
// A carried player's refreshed token (the contract's 9.3). carry() with
// PlayerToken::replaces keeps the player carried throughout: the carry made with the old
// token stands until the new one takes effect, the player's sends name the old id until
// the new FLEET_CARRY leaves and the new id from then on, and the old id's late frames are
// ignored and counted.
// ---------------------------------------------------------------------------

void runARefreshedTokenKeepsThePlayerCarried() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  cfg.carryBurst = 1;  // one carry a second, so a carry can be made to wait for its turn
  cfg.carriesPerSecond = 1;
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);
  const std::int64_t user = 7001;
  const std::int64_t first = 701, second = 702, third = 703, fourth = 704;
  carryAs(fleet, server, first, user, tokenFor(first));
  CHECK(fleet.setSpeakers(first, SpeakerList::only({uuid('s')}), SpeakerList::keep()).ok());
  CHECK_EQ(server.recvOne()[0], 47u);
  const std::uint8_t pose[] = {1, 2, 3};
  const SpatialSend update{{1, 2, 3}, uuid('a'), Bytes(pose, sizeof(pose))};
  // What a send for `id` names on the wire.
  const auto named = [&](std::int64_t id) {
    CHECK(fleet.sendActorUpdate(id, update).ok());
    const auto got = server.recvOne();
    CHECK_EQ(got[0], 128u);
    return tailIdSignedByFleet(got);
  };

  // The refreshed token's carry waits for its turn. The player is carried all the while,
  // under either id, and the sends still name the old one: the new carry has not left.
  std::vector<FleetCarryResult> viaCallback;
  const auto record = [&](const FleetCarryResult& r) { viaCallback.push_back(r); };
  CHECK(fleet.carry({tokenFor(second), second, first}, record).ok());
  fleet.pump(0);
  server.expectNothing();
  CHECK(fleet.carries(first) && fleet.carries(second));
  CHECK_EQ(fleet.carriedCount(), 1u);
  CHECK_EQ(fleet.players().size(), 1u);
  CHECK_EQ(fleet.players()[0].gameTokenId, second);
  CHECK(fleet.player(first)->carried && fleet.player(first)->pending);
  CHECK_EQ(fleet.player(first)->userId, user);
  CHECK_EQ(named(first), first);
  CHECK_EQ(named(second), first);

  // The FLEET_CARRY leaves, proved with the new token. From that moment every send for the
  // player names the new id, whichever id the game server passes.
  clock.advance(1000);
  fleet.pump(0);
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(second)).player, second);
  CHECK_EQ(named(first), second);
  CHECK_EQ(named(second), second);
  CHECK(fleet.carries(first) && fleet.carries(second));
  CHECK(viaCallback.empty());

  // The new carry took effect. The old id names nothing now; nobody was told of a release;
  // the speaker list came along under the new id.
  const std::size_t resultsBefore = seen.results.size();
  server.send(own(carryResultFrame(second, user, 0)));
  pumpUntil(fleet, [&] { return !viaCallback.empty(); });
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK(viaCallback[0].carried());
  CHECK_EQ(viaCallback[0].playerGameTokenId, second);
  CHECK_EQ(viaCallback[0].userId, user);
  CHECK_EQ(seen.results.size(), resultsBefore + 1);
  CHECK(fleet.carries(second));
  CHECK(!fleet.carries(first));
  CHECK(!fleet.player(first).has_value());
  CHECK(!fleet.player(second)->pending);
  CHECK(seen.released.empty());
  {
    const auto got = server.recvOne();
    CHECK_EQ(got[0], 47u);
    CHECK_EQ(tailId(got), second);
  }
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(first, update).error()), static_cast<int>(Errc::NotCarried));
  CHECK_EQ(named(second), second);

  // The stragglers: the old id's FLEET_RELEASED (the server ended that carry when the new
  // one took effect; it can arrive seconds late) and a late answer to an old carry. They
  // are words about the old token: ignored, and counted.
  CHECK_EQ(fleet.stats().replacedIdFrames, 0u);
  server.send(own(releasedFrame(first, 1)));
  server.send(own(carryResultFrame(first, user, 0)));
  server.send(own(carryResultFrame(first, 0, 1)));
  pumpUntil(fleet, [&] { return fleet.stats().replacedIdFrames == 3; });
  settle(fleet);
  CHECK_EQ(fleet.stats().replacedIdFrames, 3u);
  CHECK(seen.released.empty());
  CHECK(fleet.carries(second));
  CHECK_EQ(fleet.players().size(), 1u);
  CHECK_EQ(seen.results.size(), resultsBefore + 1);

  // A replacement the server refuses: the player goes back to the old id, still carried,
  // and its sends name the old id again.
  viaCallback.clear();
  clock.advance(1000);
  CHECK(fleet.carry({tokenFor(third), third, second}, record).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(third)).player, third);
  CHECK_EQ(named(second), third);
  server.send(own(carryResultFrame(third, 0, 2)));
  pumpUntil(fleet, [&] { return !viaCallback.empty(); });
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::OtherOrganization));
  CHECK_EQ(viaCallback[0].playerGameTokenId, third);
  CHECK(fleet.carries(second));
  CHECK(!fleet.carries(third));
  CHECK(!fleet.player(second)->pending);
  CHECK_EQ(fleet.players().size(), 1u);
  CHECK(seen.released.empty());
  CHECK_EQ(named(second), second);
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(third, update).error()), static_cast<int>(Errc::NotCarried));
  // The refused id is finished too: a late word about it is counted and ignored.
  server.send(own(releasedFrame(third, 1)));
  pumpUntil(fleet, [&] { return fleet.stats().replacedIdFrames == 4; });
  CHECK(fleet.carries(second));

  // The old carry ends on the server while the replacement waits (the player's sign-in
  // ended, say). The player is not dropped yet: the new carry's answer decides. It is
  // refused, so the player is carried no more, and the release is reported then.
  viaCallback.clear();
  clock.advance(1000);
  CHECK(fleet.carry({tokenFor(third), third, second}, record).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(third)).player, third);
  server.send(own(releasedFrame(second, 2)));
  pumpUntil(fleet, [&] { return fleet.stats().replacedIdFrames == 5; });
  settle(fleet);
  CHECK(seen.released.empty());
  CHECK(fleet.carries(second) && fleet.carries(third));
  server.send(own(carryResultFrame(third, 0, 2)));
  pumpUntil(fleet, [&] { return !seen.released.empty(); });
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK(!viaCallback[0].carried());
  CHECK_EQ(seen.released.size(), 1u);
  CHECK_EQ(seen.released[0].playerGameTokenId, second);
  CHECK_EQ(seen.released[0].userId, user);
  CHECK_EQ(static_cast<int>(seen.released[0].reason), static_cast<int>(FleetReleaseReason::SignInEnded));
  CHECK(!fleet.carries(second) && !fleet.carries(third));
  CHECK(fleet.players().empty());

  // A replacement nothing answers: the session cannot know which id the server holds the
  // player under, so it lets both go and the player is carried no more.
  clock.advance(1000);
  carryAs(fleet, server, first, user, tokenFor(first));
  viaCallback.clear();
  clock.advance(1000);
  CHECK(fleet.carry({tokenFor(fourth), fourth, first}, record).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(fourth)).player, fourth);
  for (int step = 0; step < 21 && viaCallback.empty(); ++step) {
    clock.advance(250);
    fleet.pump(0);
    fleet.poll();
  }
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK_EQ(static_cast<int>(viaCallback[0].status), static_cast<int>(FleetCarryStatus::TimedOut));
  CHECK_EQ(viaCallback[0].playerGameTokenId, fourth);
  CHECK(!fleet.carries(first) && !fleet.carries(fourth));
  CHECK(fleet.players().empty());
  std::vector<std::int64_t> let;
  for (const auto& got : sentSoFar(server)) {
    if (got[0] == 50) let.push_back(le::readI64(got.data() + wire::offsets::kPayload));
    else CHECK_EQ(got[0], 49u);  // the carry sent again while it waited
  }
  std::sort(let.begin(), let.end());
  CHECK((let == std::vector<std::int64_t>{first, fourth}));

  // release() while a replacement waits lets both ids go, whichever it is given.
  clock.advance(1000);
  carryAs(fleet, server, first, user, tokenFor(first));
  clock.advance(1000);
  CHECK(fleet.carry({tokenFor(fourth), fourth, first}).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(fourth)).player, fourth);
  CHECK(fleet.release(first).ok());
  let.clear();
  for (const auto& got : sentSoFar(server)) {
    CHECK_EQ(got[0], 50u);
    let.push_back(le::readI64(got.data() + wire::offsets::kPayload));
  }
  std::sort(let.begin(), let.end());
  CHECK((let == std::vector<std::int64_t>{first, fourth}));
  CHECK(fleet.players().empty());

  // `replaces` naming an id the session does not hold is a first carry. `replaces` naming
  // ANOTHER player (the answer's user id says so) releases that player's old carry on the
  // server and leaves their speaker list behind.
  clock.advance(1000);
  CHECK(fleet.carry({tokenFor(first), first, 31337}).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(first)).player, first);
  server.send(own(carryResultFrame(first, user, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(first); });
  CHECK(fleet.setSpeakers(first, SpeakerList::only({uuid('s')}), SpeakerList::keep()).ok());
  CHECK_EQ(server.recvOne()[0], 47u);
  clock.advance(1000);
  CHECK(fleet.carry({tokenFor(second), second, first}).ok());
  CHECK_EQ(readCarry(server.recvOne(), tokenFor(second)).player, second);
  server.send(own(carryResultFrame(second, user + 1, 0)));
  pumpUntil(fleet, [&] { return !fleet.carries(first); });
  CHECK(fleet.carries(second));
  CHECK_EQ(fleet.player(second)->userId, user + 1);
  {
    const auto sent = sentSoFar(server);
    CHECK_EQ(sent.size(), 1u);
    CHECK_EQ(sent[0][0], 50u);
    CHECK_EQ(le::readI64(sent[0].data() + wire::offsets::kPayload), first);
  }
}

// The sequence with sends in flight: two threads send for the player, under the old id and
// the new one alike, while the game thread carries the player with one refreshed token
// after another. One thread sends actor updates; the other sends video frames of 16
// fragments each, so that most of the time one of its sends is between its check and its
// last fragment. In the order the datagrams left: before a refreshed token's FLEET_CARRY
// every message names the id it replaces, and from that carry on every message names the
// new id, the rest of a frame that had begun included: the carry waits for the frame. (Run
// this under ThreadSanitizer too.)
//
// This case judges by the order of arrival, and reads the socket as it is. The order the
// sequence numbers give would not do: a send that took its number before the carry and
// reached the socket after it is the very fault this case is here for, and its number
// would put it back in front. So the case needs a loopback that keeps the order, as every
// real one does, and asks nothing else of it: any delay will do, because its waits pump and
// read until what they wait for has come. On a path that reorders (the head of this file
// says when netem's does) the verdict means nothing; the case counts the datagrams of one
// thread that arrived behind a later one and names them beside a verdict that fails.
// FLEET_TEST_LOOPBACK_REORDERS=1 leaves the case out for a run on such a path.
void runSendsInFlightSwitchWithTheCarry() {
  FakeServer server;
  server.start();
  FleetConfig cfg = configFor(server);
  // 150 refreshed tokens against a burst of 64 at 40 a second: on any machine some carries
  // leave from carry() and the rest wait for their turn and leave from pump(), with the
  // same order on the wire either way.
  cfg.carryBurst = 64;
  cfg.carriesPerSecond = 40;
  FleetSession fleet(cfg, crypto());
  openConnected(fleet, server);
  const std::int64_t user = 7101;
  constexpr int kTokens = 150;
  const std::int64_t base = 71000;
  carryAs(fleet, server, base, user, tokenFor(base));
  // From here the server is a bare socket: the senders never stop, so there is no moment
  // to wait for "everything sent", and this thread counts what it reads itself.
  server.forget();
  std::uint64_t taken = fleet.stats().datagramsSent;  // the capabilities and the first carry
  std::atomic<std::int64_t> oldId{base}, newId{base};
  std::atomic<bool> stop{false};
  std::atomic<int> sent{0}, notCarried{0}, otherRefusals{0};
  const std::vector<std::uint8_t> frame(16 * 1000, 0x5a);
  const auto sender = [&](int which) {
    const std::uint8_t pose[] = {1, 2, 3};
    const SpatialSend update{{1, 2, 3}, uuid('a'), Bytes(pose, sizeof(pose))};
    for (int i = 0; !stop.load(); ++i) {
      // One thread names the id the player had before the latest refresh, the other the
      // latest, and each swaps now and then.
      const std::int64_t id = ((i / 16 + which) % 2 == 0) ? oldId.load() : newId.load();
      Errc refusal = Errc::Ok;
      if (which == 0) {
        const auto st = fleet.sendActorUpdate(id, update);
        if (!st.ok()) refusal = st.error();
      } else {
        const auto st = fleet.sendVideoFrame(id, {1, 2, 3}, uuid('a'), Bytes(frame.data(), frame.size()),
                                             static_cast<std::uint16_t>(i));
        if (!st.ok()) refusal = st.error();
      }
      if (refusal == Errc::Ok) ++sent;
      else if (refusal == Errc::NotCarried) ++notCarried;  // an id from two refreshes ago
      else if (refusal != Errc::WouldBlock) ++otherRefusals;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  };
  std::thread one(sender, 0), two(sender, 1);

  // The fake server's side, on this thread: everything in the order it arrived.
  std::int64_t expected = base;  // the id the player's sends name now
  int updates = 0, wrong = 0, carries = 0;
  // Whether the path kept the order, which this case takes for granted: the video fragments
  // are one thread's and number themselves (the frame, then the fragment within it), so one
  // that arrives behind that thread's last was overtaken on the way. Counted, to be named
  // beside the verdict below.
  int overtaken = 0;
  std::int64_t lastFragment = -1;  // frame * 16 + fragment, of 2^20
  const auto drain = [&](int atMost) {
    // The bound is tested before the read: a datagram taken off the socket is always handled.
    for (int read = 0; read < atMost; ++read) {
      const auto got = server.tryRecv(1);
      if (got.empty()) break;
      ++taken;
      if (got[0] == 49) {
        const std::int64_t id = le::readI64(got.data() + wire::offsets::kPayload);
        if (id != expected) ++carries;  // a carry sent again names the same id
        expected = id;
      } else if (got[0] == 128 || got[0] == 143) {
        ++updates;
        if (tailId(got) != expected) ++wrong;
        if (got[0] == 143) {
          const std::uint8_t* header = got.data() + wire::offsets::kPayload;
          const std::int64_t fragment = ((std::int64_t{header[2]} << 8 | header[3]) << 4) + header[4];
          const std::int64_t ahead = (fragment - lastFragment) & ((1 << 20) - 1);
          if (lastFragment >= 0 && (ahead == 0 || ahead >= (1 << 19))) ++overtaken;
          else lastFragment = fragment;
        }
      }
    }
  };
  for (int n = 1; n <= kTokens; ++n) {
    const std::int64_t id = base + n;
    drain(500);
    oldId.store(newId.load());
    CHECK(fleet.carry({tokenFor(id), id, id - 1}).ok());
    newId.store(id);
    // A carry with no turn left in the rate leaves at a later pump(), so the wait pumps.
    // (It used to only read. On a machine fast enough to spend the burst, the carry never
    // left and this check failed: macOS in CI, about half the runs.)
    for (int i = 0; i < 5000 && expected != id; ++i) {
      fleet.pump(1);
      drain(200);
    }
    CHECK_EQ(expected, id);
    server.send(own(carryResultFrame(id, user, 0)));
    for (int i = 0; i < 2000 && fleet.player(id)->pending; ++i) {
      fleet.pump(1);
      fleet.poll();
      drain(200);
    }
    CHECK(!fleet.player(id)->pending);
    CHECK(fleet.carries(id));
  }
  stop.store(true);
  one.join();
  two.join();
  // Everything the three threads sent is read before the counts are: the session's own count
  // says how many datagrams left. (One the kernel dropped for want of room would be missing
  // for good; the wait gives up on it and the counts are judged without it, as they were.)
  const std::uint64_t left = fleet.stats().datagramsSent;
  const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (taken < left && std::chrono::steady_clock::now() < giveUp) drain(1000);
  drain(1000);
  CHECK_EQ(carries, kTokens);
  CHECK(updates > kTokens);
  if (overtaken != 0) {
    std::fprintf(stderr,
                 "the loopback handed over %d of %d datagrams behind a later one of the same thread: "
                 "it does not keep the order here, and this case cannot judge by it "
                 "(FLEET_TEST_LOOPBACK_REORDERS=1 leaves the case out)\n",
                 overtaken, updates);
  }
  if (wrong != 0) std::fprintf(stderr, "%d of %d messages named an id out of turn\n", wrong, updates);
  CHECK_EQ(wrong, 0);
  CHECK_EQ(otherRefusals.load(), 0);
  CHECK_EQ(fleet.players().size(), 1u);
  CHECK(fleet.carries(base + kTokens));
  CHECK_EQ(fleet.stats().sendsFailed, 0u);
}

// ---------------------------------------------------------------------------
// A carry answered "no live token" (status 1) is asked once more, a resend interval later,
// with a new time and proof: a token minted or refreshed a moment ago may not have reached
// the realtime server yet (the contract's 9.3). The answer after that settles the carry.
// ---------------------------------------------------------------------------

void runNoLiveTokenIsAskedOnceMore() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet, server);

  // Status 1, then carried on the second asking.
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  const Carry firstCarry = readCarry(server.recvOne(), kPlayerToken);
  server.send(own(carryResultFrame(kPlayerId, 0, 1)));
  pumpUntil(fleet, [&] { return fleet.stats().noLiveTokenRetries == 1; });
  settle(fleet);
  CHECK(seen.results.empty());
  CHECK(fleet.player(kPlayerId)->pending);
  CHECK_EQ(fleet.stats().noLiveTokenRetries, 1u);
  server.expectNothing();  // about 250 ms later, and no sooner
  // A second status 1 before that carry leaves answers an earlier one: nothing settles, and
  // it is not asked a third time.
  server.send(own(carryResultFrame(kPlayerId, 0, 1)));
  pumpUntil(fleet, [&] { return fleet.stats().messagesReceivedByType[51] == 2; });
  settle(fleet);
  CHECK(seen.results.empty());
  CHECK_EQ(fleet.stats().noLiveTokenRetries, 1u);
  clock.advance(249);
  fleet.pump(0);
  server.expectNothing();
  clock.advance(1);
  fleet.pump(0);
  const Carry again = readCarry(server.recvOne(), kPlayerToken);
  CHECK_EQ(again.epochMs, firstCarry.epochMs + 250);
  CHECK(again.proof != firstCarry.proof);
  server.send(own(carryResultFrame(kPlayerId, 77, 0)));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  CHECK(fleet.carries(kPlayerId));
  CHECK_EQ(seen.results.size(), 1u);
  CHECK(seen.results[0].carried());
  CHECK(fleet.release(kPlayerId).ok());
  CHECK_EQ(server.recvOne()[0], 50u);

  // Status 1 on the second asking too: the carry ends NoLiveToken, once.
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  readCarry(server.recvOne(), kPlayerToken);
  server.send(own(carryResultFrame(kPlayerId, 0, 1)));
  pumpUntil(fleet, [&] { return fleet.stats().noLiveTokenRetries == 2; });
  clock.advance(250);
  fleet.pump(0);
  readCarry(server.recvOne(), kPlayerToken);
  server.send(own(carryResultFrame(kPlayerId, 0, 1)));
  pumpUntil(fleet, [&] { return seen.results.size() == 2; });
  CHECK_EQ(seen.results.size(), 2u);
  CHECK_EQ(static_cast<int>(seen.results[1].status), static_cast<int>(FleetCarryStatus::NoLiveToken));
  CHECK_EQ(seen.results[1].wireStatus, 1u);
  CHECK(!fleet.player(kPlayerId).has_value());
  CHECK_EQ(fleet.stats().noLiveTokenRetries, 2u);
  clock.advance(1000);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();

  // Status 1 and then nothing: at the deadline the carry ends as the answer it had.
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  readCarry(server.recvOne(), kPlayerToken);
  server.send(own(carryResultFrame(kPlayerId, 0, 1)));
  pumpUntil(fleet, [&] { return fleet.stats().noLiveTokenRetries == 3; });
  for (int step = 0; step < 21 && seen.results.size() == 2; ++step) {
    clock.advance(250);
    fleet.pump(0);
    fleet.poll();
  }
  CHECK_EQ(seen.results.size(), 3u);
  CHECK_EQ(static_cast<int>(seen.results[2].status), static_cast<int>(FleetCarryStatus::NoLiveToken));
  CHECK_EQ(seen.results[2].wireStatus, 1u);
  CHECK(!fleet.player(kPlayerId).has_value());
}

// ---------------------------------------------------------------------------
// The game thread sends for its players while the network thread moves the session between
// realtime servers: the fields the two share are read and written whole (run this under
// ThreadSanitizer).
// ---------------------------------------------------------------------------

void runSendsWhileTheSessionMoves() {
  FakeServer first, second;
  first.start();
  second.start();
  FleetConfig cfg = configFor(first);
  cfg.transport.manualPump = false;
  cfg.transport.watchdogSilenceMs = 60'000;  // the watchdog reads what the receive path writes
  FleetSession fleet(cfg, crypto());
  CHECK(fleet.open().ok());

  std::atomic<bool> stop{false};
  std::atomic<int> attempts{0};
  std::thread sender([&] {
    const std::uint8_t pose[] = {1, 2, 3, 4};
    SpatialSend p;
    p.uuid = uuid('a');
    p.payload = Bytes(pose, sizeof(pose));
    while (!stop.load()) {
      (void)fleet.sendActorUpdate(kPlayerId, p);  // NotCarried or NotConnected, mostly
      (void)fleet.sendHeartbeat();
      (void)fleet.carry({kPlayerToken, kPlayerId});
      (void)fleet.release(kPlayerId);
      (void)fleet.stats();
      (void)fleet.refreshDue();
      ++attempts;
    }
  });
  // The servers answer nothing; the session is moved back and forth under the sender.
  for (int move = 0; move < 20; ++move) {
    FakeServer& to = (move % 2 == 0) ? second : first;
    CHECK(fleet.setServer(Assignment{"127.0.0.1", "", to.port}).ok());
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (std::chrono::steady_clock::now() < until) {
      fleet.poll();
      (void)first.tryRecv(1);
      (void)second.tryRecv(1);
    }
  }
  stop.store(true);
  sender.join();
  CHECK(attempts.load() > 0);
  CHECK(fleet.stats().reconnects >= 10u);
  fleet.close();
}

// ---------------------------------------------------------------------------
// The owned network thread: carries are sent again by that thread, results reach poll().
// ---------------------------------------------------------------------------

void runOwnedThread() {
  FakeServer server;
  server.start();
  FleetConfig cfg = configFor(server);
  cfg.transport.manualPump = false;
  cfg.carryResendMs = 30;
  cfg.carryTimeoutMs = 2000;
  FleetSession fleet(cfg, crypto());
  std::atomic<int> carried{0};
  std::atomic<int> texts{0};
  FleetHandlers h;
  h.carryResult = [&](const FleetCarryResult& r) {
    if (r.carried()) ++carried;
  };
  h.text = [&](const SpatialNotification&, FleetRecipients r) {
    if (!r.forSession() && r.size() == 1 && r[0] == kPlayerId) ++texts;
  };
  fleet.setHandlers(std::move(h));
  server.serves(fleet, Pumped::ByItsOwnThread);
  CHECK(fleet.open().ok());

  // Carried from another thread while the session is still connecting.
  std::thread caller([&] { CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok()); });
  caller.join();
  // The capabilities leave first. The first answer is lost: the network thread sends the
  // carry again.
  CHECK_EQ(server.recvOne()[0], 29u);
  CHECK_EQ(readCarry(server.recvOne(), kPlayerToken).player, kPlayerId);
  CHECK_EQ(readCarry(server.recvOne(), kPlayerToken).player, kPlayerId);
  server.send(own(carryResultFrame(kPlayerId, 77, 0)));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (carried.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    fleet.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(carried.load(), 1);
  CHECK(fleet.carries(kPlayerId));

  // Sends from two threads while the game thread polls a delivery. A socket may push back
  // on a burst (a full buffer, which the library reports as WouldBlock: macOS loopback does
  // where Linux does not), and a send that is pushed back is sent again. Any other refusal
  // is a defect and is named. SocketError is one of those: a fault the socket reported, which
  // sending again would hide.
  std::atomic<int> sent{0};
  std::atomic<int> pushedBack{0};
  std::atomic<int> refused{0};
  std::atomic<int> firstRefusal{0};
  auto sender = [&] {
    const std::uint8_t pose[] = {1, 2, 3, 4};
    SpatialSend p;
    p.uuid = uuid('a');
    p.payload = Bytes(pose, sizeof(pose));
    for (int i = 0; i < 50; ++i) {
      for (int attempt = 0; attempt < 500; ++attempt) {
        const auto st = fleet.sendGenericSpatial(kPlayerId, p);
        if (st.ok()) {
          ++sent;
          break;
        }
        if (st.error() != Errc::WouldBlock) {
          int none = 0;
          firstRefusal.compare_exchange_strong(none, static_cast<int>(st.error()));
          ++refused;
          break;
        }
        ++pushedBack;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  };
  std::thread one(sender), two(sender);
  const std::uint8_t body[] = {'h', 'i'};
  server.send(own(deliveryFrame({kPlayerId}, unsignedNote(wire::MessageType::ClientTextNotification, Bytes(body, 2), 1))));
  one.join();
  two.join();
  while (texts.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    fleet.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(texts.load(), 1);
  const auto stats = fleet.stats();
  if (refused.load() != 0 || sent.load() != 100 || stats.sendsFailed != 0) {
    std::fprintf(stderr,
                 "sends: %d sent, %d pushed back, %d refused (first refusal: %s); the session "
                 "counts %llu deferred and %llu failed\n",
                 sent.load(), pushedBack.load(), refused.load(),
                 errcName(static_cast<Errc>(firstRefusal.load())),
                 static_cast<unsigned long long>(stats.sendsDeferred),
                 static_cast<unsigned long long>(stats.sendsFailed));
  }
  CHECK_EQ(refused.load(), 0);
  CHECK_EQ(sent.load(), 100);
  // No send of the session's, these or its own, met a socket fault.
  CHECK_EQ(stats.sendsFailed, 0u);
  CHECK_EQ(fleet.stats().messagesSentByType[140], 100u);
  fleet.close();
}

}  // namespace

int main() {
  runCarrySendRelease();
  runEverySendForAPlayer();
  runOnlyFourMessagesUnderTheSessionsId();
  runCarryResendAndDeadline();
  runCarriesArePaced();
  runCarryStatuses();
  runReleasedReasons();
  runRefreshedTokenReplacesEntry();
  runDeliveryAndPlainState();
  runBothSignedForms();
  runLargestDatagram();
  runFleetOwnStateCapability();
  runAnotherServerCarriesEveryPlayerAgain();
  runSameServerKeepsItsCarries();
  runMoveOrderWaitsForARefresh();
  runSessionReleasedEndsTheCarries();
  runSetToken();
  runErrorFramesChangeNothing();
  runProofRefusedConverges();
  runSilenceTimeoutDefaults();
  runEverySilenceTimeoutFires();
  runAnIdleSessionIsNeverTakenForGone();
  runTwoLostAcknowledgementsAreNotSilence();
  runSilenceMakesARefreshDue();
  runSilenceAsksTheGameServerForAServer();
  runTheServerEndsTheSession();
  runOnlyAVerifiedReleaseEndsTheSession();
  runALostReleaseLeavesTheSilenceTimeout();
  runSendFaultIsNamed();
  runOnlyTheSessionsOwnAppIsDispatched();
  runVerifiedRefusals();
  runARefreshedTokenKeepsThePlayerCarried();
  if (const char* reorders = std::getenv("FLEET_TEST_LOOPBACK_REORDERS"); reorders && reorders[0] == '1') {
    std::puts("runSendsInFlightSwitchWithTheCarry left out: FLEET_TEST_LOOPBACK_REORDERS=1");
  } else {
    runSendsInFlightSwitchWithTheCarry();
  }
  runNoLiveTokenIsAskedOnceMore();
  runSendsWhileTheSessionMoves();
  runOwnedThread();
  std::puts("fleet_session_test OK");
  return 0;
}
