// What a player's own Connection does with what it is sent (0.95.0), against a loopback UDP
// "server" in manual-pump mode:
//
//  - every form the realtime servers send a player's session still arrives: each
//    notification type alone with its own HMAC, as a member of a plain bundle, and as a
//    member of a signed bundle; channel notifications and error frames, which carry no
//    HMAC of their own, either way;
//  - a long-spatial notification that nothing signed, and one that names another app, is
//    dropped, counted (Stats::unsignedNotifications, Stats::wrongApp) and reaches no handler;
//  - a TOKEN_EXPIRED that nothing verified does not reassign: the session provider is asked
//    for a fresh token, at most once in kTokenCheckIntervalMs, and its answer decides;
//  - a receive pass and disconnect() end whatever is still arriving.
//
// Nothing here depends on how long a datagram takes to cross the loopback (Linux hands one
// to its receiver before sendto() returns; macOS does not, and refuses a burst with ENOBUFS).
// `deliver` sends one datagram and pumps until the connection has read it, so every check
// that follows it reads a settled count.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/replication/connection.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

const std::string kToken(64, 't');
constexpr std::int64_t kApp = 7;
constexpr std::int64_t kOtherApp = 8;

wire::Token64 key() { return *wire::Token64::fromString(kToken); }
const core::ICrypto& crypto() { return core::opensslCrypto(); }

core::ActorUuid uuid(char fill) {
  core::ActorUuid u;
  std::memset(u.data(), fill, 32);
  return u;
}

struct FakeServer {
  int fd = -1;
  int port = 0;
  sockaddr_in client{};
  socklen_t clientLen = 0;

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
  }
  ~FakeServer() {
    if (fd >= 0) ::close(fd);
  }

  std::vector<std::uint8_t> recvOne(int timeoutMs = 5000) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    clientLen = sizeof(client);
    const ssize_t n =
        ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&client), &clientLen);
    CHECK(n > 0);
    return std::vector<std::uint8_t>(buf, buf + (n > 0 ? n : 0));
  }

  /// One attempt, for the flood: whether the kernel took the datagram.
  bool send(const std::vector<std::uint8_t>& datagram) const {
    return ::sendto(fd, datagram.data(), datagram.size(), 0,
                    reinterpret_cast<const sockaddr*>(&client), clientLen) ==
           static_cast<ssize_t>(datagram.size());
  }
};

struct Provider final : ISessionProvider {
  int port;
  int assigns = 0;
  int refreshes = 0;
  bool refreshFails = false;
  bool authorizedOnCurrent = true;
  explicit Provider(int p) : port(p) {}
  Result<Assignment> assignServer() override {
    ++assigns;
    return Assignment{"127.0.0.1", "", port};
  }
  Result<TokenInfo> refreshToken() override { return refreshToken(nullptr); }
  Result<TokenInfo> refreshToken(const Assignment*) override {
    ++refreshes;
    if (refreshFails) return Errc::Rejected;
    TokenInfo t{kToken, 42, 0};
    t.authorizedOnCurrentServer = authorizedOnCurrent;
    return t;
  }
};

struct ManualClock final : core::IClock {
  std::int64_t epoch = 1'700'000'000'000LL;
  std::int64_t mono = 10'000;
  std::int64_t epochMillis() const override { return epoch; }
  std::int64_t monotonicMillis() const override { return mono; }
};

// ---- Frames, as the realtime servers make them -------------------------------------------

/// A long-spatial notification with its own HMAC (containsAuth 1), epoch millis in the tail.
std::vector<std::uint8_t> signedNote(wire::MessageType type, std::int64_t appId = kApp,
                                     std::size_t payloadSize = 12, std::uint8_t seq = 5) {
  std::vector<std::uint8_t> payload(payloadSize, 0x5a);
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = appId;
  p.chunk = {1, 2, 3};
  p.distance = 8;
  p.uuid = uuid('b');
  p.payload = Bytes(payload.data(), payload.size());
  p.gameTokenId = 1'700'000'000'000LL;
  p.sequence = seq;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(payload.size()));
  auto n = wire::encodeLongSpatial(crypto(), p, key(), MutableBytes(buf.data(), buf.size()));
  CHECK(n.ok());
  buf.resize(n.value());
  return buf;
}

/// The same notification with no HMAC of its own (containsAuth 0): what a member of a signed
/// bundle is, and what nothing vouches for anywhere else.
std::vector<std::uint8_t> bare(std::vector<std::uint8_t> note) {
  note[wire::offsets::kContainsAuth] = 0;
  const std::size_t hmacAt = note.size() - wire::kTailWithHmac;
  note.erase(note.begin() + static_cast<std::ptrdiff_t>(hmacAt),
             note.begin() + static_cast<std::ptrdiff_t>(hmacAt + wire::kHmacTagSize));
  return note;
}

/// A channel notification (18) or channel audio (36): no HMAC in its layout.
std::vector<std::uint8_t> channelNote(std::uint8_t type) {
  const std::uint8_t payload[] = {'h', 'i'};
  std::vector<std::uint8_t> f(wire::channel::kHeaderSize + sizeof(payload) +
                              wire::channel::kNotificationTailSize);
  f[0] = type;
  le::writeI64(f.data() + wire::channel::kChannelIdOffset, 55);
  std::memset(f.data() + wire::channel::kUuidOffset, 'c', 32);
  le::writeU16(f.data() + wire::channel::kPayloadLenOffset, sizeof(payload));
  std::memcpy(f.data() + wire::channel::kPayloadOffset, payload, sizeof(payload));
  le::writeI64(f.data() + wire::channel::kPayloadOffset + sizeof(payload), 1'700'000'000'001LL);
  f[f.size() - 1] = 9;
  return f;
}

std::vector<std::uint8_t> bundleOf(std::uint8_t type,
                                   const std::vector<std::vector<std::uint8_t>>& members) {
  std::vector<std::uint8_t> dg = {type};
  for (const auto& m : members) {
    dg.push_back(static_cast<std::uint8_t>(m.size() & 0xff));
    dg.push_back(static_cast<std::uint8_t>(m.size() >> 8));
    dg.insert(dg.end(), m.begin(), m.end());
  }
  return dg;
}

/// MESSAGE_BUNDLE (2): no HMAC of its own.
std::vector<std::uint8_t> plainBundle(const std::vector<std::vector<std::uint8_t>>& members) {
  return bundleOf(2, members);
}

/// MESSAGE_BUNDLE_SIGNED (30): one HMAC over the datagram, keyed with the session's token.
std::vector<std::uint8_t> signedBundle(const std::vector<std::vector<std::uint8_t>>& members) {
  std::vector<std::uint8_t> dg = bundleOf(30, members);
  const std::size_t body = dg.size();
  dg.resize(body + wire::kHmacTagSize);
  CHECK(wire::spatialHmac(crypto(), Bytes(dg.data(), body), key(), dg.data() + body));
  return dg;
}

// ---- A player's connection, and what its handlers were given -----------------------------

struct Seen {
  std::map<int, int> spatialByType;
  int spatial = 0;
  int channelMessages = 0;
  int channelAudio = 0;
  std::vector<GenericError> errors;
  std::vector<std::int64_t> apps;
};

Handlers recording(Seen& seen) {
  Handlers h;
  h.any = [&seen](const SpatialNotification& n) {
    ++seen.spatial;
    ++seen.spatialByType[static_cast<int>(n.type)];
    seen.apps.push_back(n.appId);
  };
  h.channelMessage = [&seen](const ChannelNotification&) { ++seen.channelMessages; };
  h.channelAudio = [&seen](const ChannelNotification&) { ++seen.channelAudio; };
  h.genericError = [&seen](const GenericError& e) { seen.errors.push_back(e); };
  return h;
}

Config playerConfig() {
  Config cfg;
  cfg.appId = kApp;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;
  cfg.bundleSends = false;
  cfg.advertiseCapabilities = false;
  return cfg;
}

/// Connect, and let the server learn the client's address from a heartbeat.
void open(Connection& conn, FakeServer& server) {
  CHECK(conn.connect().ok());
  conn.pump(0);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);
}

/// Send one datagram and pump until the connection has read it (Stats::datagramsReceived
/// moved), then dispatch what it queued. A send the kernel refused for a full buffer is made
/// again. When this returns, the datagram was delivered or counted as dropped.
void deliver(Connection& conn, const FakeServer& server, const std::vector<std::uint8_t>& datagram) {
  const std::uint64_t before = conn.stats().datagramsReceived;
  bool sent = false;
  for (int i = 0; i < 1000 && conn.stats().datagramsReceived == before; ++i) {
    if (!sent) sent = server.send(datagram);
    conn.pump(sent ? 20 : 1);
  }
  CHECK(sent);
  CHECK_EQ(conn.stats().datagramsReceived, before + 1);
  conn.poll();
}

const wire::MessageType kNotificationTypes[] = {
    wire::MessageType::ActorUpdateNotification,  wire::MessageType::VoxelUpdateNotification,
    wire::MessageType::ClientAudioNotification,  wire::MessageType::ClientTextNotification,
    wire::MessageType::ClientEventNotification,  wire::MessageType::ServerEventNotification,
    wire::MessageType::GenericSpatial1,          wire::MessageType::SingleActorMessage,
    wire::MessageType::ClientVideoNotification,  wire::MessageType::ActorLeftNotification,
    wire::MessageType::ChunkChangedNotification,
};
constexpr int kTypeCount = static_cast<int>(sizeof(kNotificationTypes) / sizeof(kNotificationTypes[0]));

// ---------------------------------------------------------------------------
// Everything the realtime servers send a player's session, in every form they send it,
// arrives, and nothing is counted as dropped. These are the forms the live suites receive:
// before the capabilities land, each notification with its own HMAC, alone or in a plain
// bundle; after, the members of a signed bundle, and a frame too large for one alone with
// its own HMAC. Channel notifications and error frames have no room for an HMAC.
// ---------------------------------------------------------------------------
void runWhatACorrectServerSendsArrives() {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<Provider>(server.port);
  Connection conn(playerConfig(), provider, crypto());
  Seen seen;
  conn.setHandlers(recording(seen));
  open(conn, server);

  int expected = 0;
  for (const wire::MessageType type : kNotificationTypes) {
    const auto note = signedNote(type);
    deliver(conn, server, note);                                    // alone, its own HMAC
    deliver(conn, server, plainBundle({note, note}));               // a plain bundle's members
    deliver(conn, server, signedBundle({bare(note), bare(note)}));  // a signed bundle's members
    deliver(conn, server, signedBundle({bare(note)}));              // a signed bundle of one
    expected += 6;
    CHECK_EQ(seen.spatial, expected);
    CHECK_EQ(seen.spatialByType[static_cast<int>(type)], 6);
  }

  // A video fragment too large to be a bundle's member travels alone with its own HMAC.
  deliver(conn, server, signedNote(wire::MessageType::ClientVideoNotification, kApp, 1100));
  CHECK_EQ(seen.spatial, ++expected);

  // Channel notifications: plain to a session that has not advertised, in a signed bundle
  // to one that has.
  for (const std::uint8_t type : {std::uint8_t{18}, std::uint8_t{36}}) {
    deliver(conn, server, channelNote(type));
    deliver(conn, server, plainBundle({channelNote(type)}));
    deliver(conn, server, signedBundle({channelNote(type)}));
  }
  CHECK_EQ(seen.channelMessages, 3);
  CHECK_EQ(seen.channelAudio, 3);

  // A refusal: alone and unsigned, as a player's session is sent every one today. One
  // inside a signed bundle is marked verified.
  const std::vector<std::uint8_t> unauthorized = {3, 200, 7};
  deliver(conn, server, unauthorized);
  deliver(conn, server,
          plainBundle({unauthorized, signedNote(wire::MessageType::ClientTextNotification)}));
  deliver(conn, server, signedBundle({unauthorized}));
  CHECK_EQ(seen.spatial, ++expected);
  CHECK_EQ(seen.errors.size(), 3u);
  int verified = 0;
  for (const GenericError& e : seen.errors) {
    CHECK_EQ(static_cast<int>(e.code), 7);
    CHECK_EQ(e.sequence, 200u);
    if (e.verified) ++verified;
  }
  CHECK_EQ(verified, 1);
  CHECK(seen.errors.back().verified);

  const auto s = conn.stats();
  CHECK_EQ(s.wrongApp, 0u);
  CHECK_EQ(s.unsignedNotifications, 0u);
  CHECK_EQ(s.hmacFailures, 0u);
  CHECK_EQ(s.malformed, 0u);
  CHECK_EQ(s.unverifiedTokenExpired, 0u);
  CHECK_EQ(s.reconnects, 0u);
  CHECK_EQ(s.messagesReceived, static_cast<std::uint64_t>(expected + 6 + 3));
  for (const std::int64_t app : seen.apps) CHECK_EQ(app, kApp);
  CHECK_EQ(provider->assigns, 1);
  CHECK_EQ(provider->refreshes, 0);
  conn.disconnect();
}

// ---------------------------------------------------------------------------
// What is dropped: a long-spatial notification that nothing signed, and one that names
// another app however it was signed. Each is counted, reaches no handler, counts as no
// received message and answers no awaited send. What shared its datagram is not affected.
// ---------------------------------------------------------------------------
void runWhatIsDropped() {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<Provider>(server.port);
  Connection conn(playerConfig(), provider, crypto());
  Seen seen;
  conn.setHandlers(recording(seen));
  open(conn, server);

  const auto mine = signedNote(wire::MessageType::ActorUpdateNotification);
  const auto others = signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp);
  std::uint64_t unsignedCount = 0;
  std::uint64_t wrongApp = 0;
  int delivered = 0;

  // Nothing signed it: alone, and beside a signed one in a plain bundle.
  deliver(conn, server, bare(mine));
  CHECK_EQ(conn.stats().unsignedNotifications, ++unsignedCount);
  deliver(conn, server, plainBundle({bare(mine), mine, bare(mine)}));
  CHECK_EQ(conn.stats().unsignedNotifications, unsignedCount += 2);
  CHECK_EQ(seen.spatial, ++delivered);

  // Each type, unsigned and alone: none is taken.
  for (const wire::MessageType type : kNotificationTypes) {
    deliver(conn, server, bare(signedNote(type)));
    CHECK_EQ(conn.stats().unsignedNotifications, ++unsignedCount);
  }
  CHECK_EQ(seen.spatial, delivered);

  // Another app's, with an HMAC of its own that holds: alone, and in a plain bundle.
  deliver(conn, server, others);
  CHECK_EQ(conn.stats().wrongApp, ++wrongApp);
  deliver(conn, server, plainBundle({others, mine}));
  CHECK_EQ(conn.stats().wrongApp, ++wrongApp);
  CHECK_EQ(seen.spatial, ++delivered);

  // Another app's inside a signed bundle that verifies, beside this app's.
  deliver(conn, server, signedBundle({bare(others), bare(mine), bare(others)}));
  CHECK_EQ(conn.stats().wrongApp, wrongApp += 2);
  CHECK_EQ(seen.spatial, ++delivered);

  // Each type of another app, signed: none is taken.
  for (const wire::MessageType type : kNotificationTypes) {
    deliver(conn, server, signedNote(type, kOtherApp));
    CHECK_EQ(conn.stats().wrongApp, ++wrongApp);
  }
  CHECK_EQ(seen.spatial, delivered);

  // Another app's that nothing signed is counted once, as unsigned: the signature is read
  // first, and without one the app it names is nobody's word.
  deliver(conn, server, bare(others));
  CHECK_EQ(conn.stats().unsignedNotifications, ++unsignedCount);
  CHECK_EQ(conn.stats().wrongApp, wrongApp);

  // A bad HMAC is what it always was.
  auto tampered = mine;
  tampered[wire::offsets::kPayload] ^= 0xff;
  deliver(conn, server, tampered);
  CHECK_EQ(conn.stats().hmacFailures, 1u);

  // None of them answers an awaited send: an echo of another app, and an unsigned one, with
  // the send's own sequence number and actor.
  SpatialSend send;
  send.chunk = {1, 2, 3};
  send.uuid = uuid('b');
  const std::uint8_t pose[] = {1, 2, 3, 4};
  send.payload = Bytes(pose, sizeof(pose));
  const auto seq = conn.sendActorUpdate(send);
  CHECK(seq.ok());
  CHECK_EQ(server.recvOne()[0], 128u);
  deliver(conn, server, signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp, 12, seq.value()));
  deliver(conn, server, bare(signedNote(wire::MessageType::ActorUpdateNotification, kApp, 12, seq.value())));
  CHECK_EQ(conn.stats().wrongApp, ++wrongApp);
  CHECK_EQ(conn.stats().unsignedNotifications, ++unsignedCount);
  // Both were read and dropped before this wait begins, so it has nothing to find.
  CHECK(!conn.waitForSequence(seq.value(), send.uuid, 1).acknowledged);
  // The real echo does answer it.
  const auto echo = signedNote(wire::MessageType::ActorUpdateNotification, kApp, 12, seq.value());
  for (int i = 0; i < 1000 && !server.send(echo); ++i) conn.pump(1);
  CHECK(conn.waitForSequence(seq.value(), send.uuid, 10000).acknowledged);
  CHECK_EQ(seen.spatial, ++delivered);

  const auto s = conn.stats();
  CHECK_EQ(s.messagesReceived, static_cast<std::uint64_t>(delivered));
  CHECK_EQ(s.malformed, 0u);
  for (const std::int64_t app : seen.apps) CHECK_EQ(app, kApp);
  CHECK_EQ(provider->assigns, 1);
  conn.disconnect();
}

// ---------------------------------------------------------------------------
// The cases a player's connection still takes on purpose.
// ---------------------------------------------------------------------------
void runWhatIsStillTaken() {
  // Verification switched off (Config::verifyNotifications): the application chose to take
  // notifications unchecked, so an unsigned one arrives, as one with a bad HMAC does.
  // Another app's is still not this connection's.
  {
    FakeServer server;
    server.start();
    auto provider = std::make_shared<Provider>(server.port);
    Config cfg = playerConfig();
    cfg.verifyNotifications = false;
    Connection conn(cfg, provider, crypto());
    Seen seen;
    conn.setHandlers(recording(seen));
    open(conn, server);
    const auto mine = signedNote(wire::MessageType::ActorUpdateNotification);
    deliver(conn, server, bare(mine));
    deliver(conn, server, plainBundle({bare(mine), mine}));
    deliver(conn, server, signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp));
    CHECK_EQ(seen.spatial, 3);
    CHECK_EQ(conn.stats().wrongApp, 1u);
    CHECK_EQ(conn.stats().unsignedNotifications, 0u);
    conn.disconnect();
  }
  // A connection that was given no app (Config::appId 0) has nothing to compare.
  {
    FakeServer server;
    server.start();
    auto provider = std::make_shared<Provider>(server.port);
    Config cfg = playerConfig();
    cfg.appId = 0;
    Connection conn(cfg, provider, crypto());
    Seen seen;
    conn.setHandlers(recording(seen));
    open(conn, server);
    deliver(conn, server, signedNote(wire::MessageType::ActorUpdateNotification, kApp));
    deliver(conn, server, signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp));
    CHECK_EQ(seen.spatial, 2);
    CHECK_EQ(conn.stats().wrongApp, 0u);
    // It still takes nothing unsigned.
    deliver(conn, server, bare(signedNote(wire::MessageType::ActorUpdateNotification, kApp)));
    CHECK_EQ(conn.stats().unsignedNotifications, 1u);
    CHECK_EQ(seen.spatial, 2);
    conn.disconnect();
  }
  // A signed notification that names no app (0) names no other app.
  {
    FakeServer server;
    server.start();
    auto provider = std::make_shared<Provider>(server.port);
    Connection conn(playerConfig(), provider, crypto());
    Seen seen;
    conn.setHandlers(recording(seen));
    open(conn, server);
    deliver(conn, server, signedNote(wire::MessageType::ServerEventNotification, 0));
    deliver(conn, server, signedBundle({bare(signedNote(wire::MessageType::ServerEventNotification, 0))}));
    CHECK_EQ(seen.spatial, 2);
    CHECK_EQ(conn.stats().wrongApp, 0u);
    conn.disconnect();
  }
}

// ---------------------------------------------------------------------------
// TOKEN_EXPIRED. Verified (a member of a signed bundle whose HMAC held), it reassigns, as it
// did. Unverified (3 bytes alone, which is how a player's session is sent every refusal
// today), it reassigns nothing: the session provider is asked for a fresh token, once in
// kTokenCheckIntervalMs at most, and what it answers decides. Either way the handler is
// told, and an awaited send that the frame names is settled.
// ---------------------------------------------------------------------------

/// One more pass of housekeeping, which is where a connection acts on what poll() read.
void housekeep(Connection& conn) {
  conn.pump(0);
  conn.poll();
}

/// After a reassignment: connected again, and the server knows the new socket.
void reopen(Connection& conn, FakeServer& server) {
  conn.pump(0);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);
}

void runTokenExpired() {
  const std::vector<std::uint8_t> expired = {3, 9, 32};

  FakeServer server;
  server.start();
  auto provider = std::make_shared<Provider>(server.port);
  ManualClock clock;
  Connection conn(playerConfig(), provider, crypto(), clock);
  Seen seen;
  conn.setHandlers(recording(seen));
  open(conn, server);
  CHECK_EQ(provider->assigns, 1);

  // A hundred of them, alone and in a plain bundle: one question to the provider, which
  // says the fresh token is known to this server, so nothing moves.
  for (int i = 0; i < 50; ++i) {
    deliver(conn, server, expired);
    deliver(conn, server, plainBundle({expired}));
  }
  housekeep(conn);
  CHECK_EQ(seen.errors.size(), 100u);
  for (const GenericError& e : seen.errors) {
    CHECK(!e.verified);
    CHECK_EQ(static_cast<int>(e.code), 32);
  }
  CHECK_EQ(conn.stats().unverifiedTokenExpired, 100u);
  CHECK_EQ(provider->refreshes, 1);
  CHECK_EQ(provider->assigns, 1);
  CHECK_EQ(conn.stats().reconnects, 0u);
  CHECK_EQ(conn.sessionEpoch(), 1u);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));

  // One millisecond short of the interval: still one question.
  clock.mono += kTokenCheckIntervalMs - 1;
  deliver(conn, server, expired);
  housekeep(conn);
  CHECK_EQ(provider->refreshes, 1);

  // At the interval: a second question. This time the provider cannot say: nothing changes.
  clock.mono += 1;
  provider->refreshFails = true;
  deliver(conn, server, expired);
  housekeep(conn);
  CHECK_EQ(provider->refreshes, 2);
  CHECK_EQ(provider->assigns, 1);
  CHECK_EQ(conn.stats().reconnects, 0u);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));

  // The provider says the fresh token is not known to this server: that answer, which is
  // the Application API's, reassigns, as it does after a refresh that was due.
  clock.mono += kTokenCheckIntervalMs;
  provider->refreshFails = false;
  provider->authorizedOnCurrent = false;
  deliver(conn, server, expired);
  housekeep(conn);
  CHECK_EQ(provider->refreshes, 3);
  CHECK_EQ(provider->assigns, 2);
  CHECK_EQ(conn.stats().reconnects, 1u);
  CHECK_EQ(seen.errors.size(), 103u);
  provider->authorizedOnCurrent = true;
  reopen(conn, server);

  // Verified: it reassigns at once, and asks the provider for no token.
  deliver(conn, server, signedBundle({expired}));
  housekeep(conn);
  CHECK_EQ(provider->assigns, 3);
  CHECK_EQ(provider->refreshes, 3);
  CHECK_EQ(seen.errors.size(), 104u);
  CHECK(seen.errors.back().verified);
  CHECK_EQ(conn.stats().reconnects, 2u);
  CHECK_EQ(conn.stats().unverifiedTokenExpired, 103u);
  reopen(conn, server);

  // A signed bundle whose HMAC does not hold is dropped whole: its TOKEN_EXPIRED reaches
  // nobody and moves nothing.
  auto forged = signedBundle({expired});
  forged.back() ^= 0x01;
  deliver(conn, server, forged);
  housekeep(conn);
  CHECK_EQ(conn.stats().hmacFailures, 1u);
  CHECK_EQ(seen.errors.size(), 104u);
  CHECK_EQ(provider->assigns, 3);

  // An unverified refusal still settles the awaited send it names.
  SpatialSend send;
  send.chunk = {1, 2, 3};
  send.uuid = uuid('a');
  const auto seq = conn.sendText(send);
  CHECK(seq.ok());
  CHECK_EQ(server.recvOne()[0], 136u);
  const std::vector<std::uint8_t> refusal = {3, seq.value(), 32};
  for (int i = 0; i < 1000 && !server.send(refusal); ++i) conn.pump(1);
  const auto outcome = conn.waitForSequence(seq.value(), send.uuid, 10000);
  CHECK(outcome.error.has_value());
  CHECK_EQ(static_cast<int>(*outcome.error), 32);
  CHECK_EQ(provider->assigns, 3);
  conn.disconnect();

  // An application that switched the refresh off (refreshLeadMs 0) manages its token
  // itself: the provider is not asked, and the frame reaches the handler.
  {
    FakeServer other;
    other.start();
    auto own = std::make_shared<Provider>(other.port);
    Config cfg = playerConfig();
    cfg.refreshLeadMs = 0;
    Connection quiet(cfg, own, crypto());
    Seen heard;
    quiet.setHandlers(recording(heard));
    open(quiet, other);
    deliver(quiet, other, expired);
    housekeep(quiet);
    CHECK_EQ(heard.errors.size(), 1u);
    CHECK_EQ(own->refreshes, 0);
    CHECK_EQ(own->assigns, 1);
    CHECK_EQ(quiet.stats().unverifiedTokenExpired, 1u);
    CHECK_EQ(quiet.stats().reconnects, 0u);
    quiet.disconnect();
  }
}

// ---------------------------------------------------------------------------
// A refused keepalive (for realtime servers v0.51.0). Where the player has no access the
// server answers every keepalive [3][sequence][UNAUTHORIZED], unsigned, every few seconds.
// The connection makes one report for the actor and the place (Handlers::notPresent, and
// Connection::notPresent to read), and ends it once: when a keepalive there goes
// kKeepaliveAnswerWaitMs without a refusal or the actor's own update comes back, when the
// actor's next keepalive or update names another chunk, or when its keepalives stop. It
// changes nothing else: not the state, not the assignment, not the token.
// ---------------------------------------------------------------------------

struct Presence {
  std::vector<ActorNotPresent> made;
  std::vector<std::pair<ActorNotPresent, NotPresentEnd>> ended;
};

Handlers recording(Seen& seen, Presence& presence) {
  Handlers h = recording(seen);
  h.notPresent = [&presence](const ActorNotPresent& report) { presence.made.push_back(report); };
  h.notPresentEnded = [&presence](const ActorNotPresent& report, NotPresentEnd why) {
    presence.ended.emplace_back(report, why);
  };
  return h;
}

/// Send a keepalive and let the server read it. Returns its sequence number.
std::uint8_t keepalive(Connection& conn, FakeServer& server, const wire::ChunkCoord& chunk,
                       const core::ActorUuid& actor) {
  const auto seq = conn.sendHeartbeat(chunk, actor);
  CHECK(seq.ok());
  const auto got = server.recvOne();
  CHECK_EQ(got[0], 26u);
  CHECK_EQ(got.back(), seq.value());
  return seq.value();
}

/// The server's refusal of the message with that sequence number, as a player's session is
/// sent it: 3 bytes, alone, unsigned.
void refuse(Connection& conn, FakeServer& server, std::uint8_t sequence, std::uint8_t code = 7) {
  deliver(conn, server, {3, sequence, code});
}

void runARefusedKeepaliveIsReportedOnce() {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<Provider>(server.port);
  ManualClock clock;
  Connection conn(playerConfig(), provider, crypto(), clock);
  Seen seen;
  Presence presence;
  conn.setHandlers(recording(seen, presence));
  open(conn, server);

  const wire::ChunkCoord here{10, 0, -4};
  const wire::ChunkCoord there{11, 0, -4};
  const core::ActorUuid actor = uuid('p');
  const auto unchanged = [&] {
    CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));
    CHECK_EQ(provider->assigns, 1);
    CHECK_EQ(provider->refreshes, 0);
    CHECK_EQ(conn.stats().reconnects, 0u);
    CHECK_EQ(conn.sessionEpoch(), 1u);
  };

  // An accepted keepalive is answered with nothing, and nothing is reported.
  keepalive(conn, server, here, actor);
  conn.pump(0);
  conn.poll();
  CHECK(presence.made.empty());
  CHECK(!conn.notPresent(actor).has_value());

  // The first refused keepalive: one report, readable, and nothing else changed.
  const std::uint8_t first = keepalive(conn, server, here, actor);
  refuse(conn, server, first);
  CHECK_EQ(presence.made.size(), 1u);
  CHECK(presence.made[0].uuid == actor);
  CHECK(presence.made[0].chunk == here);
  CHECK_EQ(presence.made[0].sequence, first);
  CHECK(!presence.made[0].verified);  // 3 bytes that nothing signed
  CHECK_EQ(presence.made[0].playerGameTokenId, 0);
  CHECK_EQ(seen.errors.size(), 1u);
  CHECK(seen.errors[0].keepalive);
  CHECK(!seen.errors[0].verified);
  CHECK(conn.notPresent(actor).has_value());
  CHECK(conn.notPresent(actor)->chunk == here);
  CHECK(!conn.notPresent(uuid('q')).has_value());
  unchanged();

  // The keepalives go on at the game's own rate, each refused: every refusal reaches
  // genericError, marked as a keepalive's, and the report is not made again.
  for (int i = 0; i < 20; ++i) {
    clock.mono += 2000;
    refuse(conn, server, keepalive(conn, server, here, actor));
  }
  CHECK_EQ(presence.made.size(), 1u);
  CHECK(presence.ended.empty());
  CHECK_EQ(seen.errors.size(), 21u);
  for (const GenericError& e : seen.errors) CHECK(e.keepalive);
  CHECK_EQ(conn.stats().keepalivesRefused, 21u);
  CHECK_EQ(conn.stats().notPresentReports, 1u);
  CHECK(conn.notPresent(actor).has_value());
  unchanged();

  // One refusal lost on the way is not acceptance: the next keepalive, 2 seconds later, is
  // refused before the lost one's 3 seconds are up.
  clock.mono += 2000;
  keepalive(conn, server, here, actor);  // its refusal never arrives
  clock.mono += 2000;
  refuse(conn, server, keepalive(conn, server, here, actor));
  clock.mono += 1500;  // 3.5 s after the unanswered one
  conn.poll();
  CHECK(presence.ended.empty());
  CHECK(conn.notPresent(actor).has_value());

  // A refusal of something that is not a keepalive is not one: an actor update's.
  SpatialSend update;
  update.chunk = here;
  update.uuid = actor;
  const std::uint8_t pose[] = {1, 2, 3, 4};
  update.payload = Bytes(pose, sizeof(pose));
  const auto updated = conn.sendActorUpdate(update);
  CHECK(updated.ok());
  CHECK_EQ(server.recvOne()[0], 128u);
  refuse(conn, server, updated.value());
  CHECK(!seen.errors.back().keepalive);
  CHECK_EQ(conn.stats().keepalivesRefused, 22u);
  CHECK_EQ(presence.made.size(), 1u);

  // Access granted: the next keepalive is not refused. The report ends 3,000 ms after it
  // left, and not a millisecond sooner.
  clock.mono += 2000;
  keepalive(conn, server, here, actor);
  clock.mono += kKeepaliveAnswerWaitMs - 1;
  conn.pump(0);
  conn.poll();
  CHECK(presence.ended.empty());
  clock.mono += 1;
  conn.poll();
  CHECK_EQ(presence.ended.size(), 1u);
  CHECK(presence.ended[0].first.uuid == actor);
  CHECK(presence.ended[0].first.chunk == here);
  CHECK_EQ(presence.ended[0].first.sequence, first);
  CHECK_EQ(static_cast<int>(presence.ended[0].second), static_cast<int>(NotPresentEnd::Accepted));
  CHECK(!conn.notPresent(actor).has_value());
  conn.poll();
  CHECK_EQ(presence.ended.size(), 1u);  // once

  // Refused there again later: a new report.
  clock.mono += 2000;
  refuse(conn, server, keepalive(conn, server, here, actor));
  CHECK_EQ(presence.made.size(), 2u);

  // The actor moves: its next keepalive names another chunk, and the report ends. Nothing
  // is known of the new place until a keepalive there is answered.
  clock.mono += 2000;
  const std::uint8_t moved = keepalive(conn, server, there, actor);
  conn.poll();
  CHECK_EQ(presence.ended.size(), 2u);
  CHECK(presence.ended[1].first.chunk == here);
  CHECK_EQ(static_cast<int>(presence.ended[1].second), static_cast<int>(NotPresentEnd::Moved));
  CHECK(!conn.notPresent(actor).has_value());
  refuse(conn, server, moved);
  CHECK_EQ(presence.made.size(), 3u);
  CHECK(presence.made[2].chunk == there);
  CHECK_EQ(presence.made[2].sequence, moved);

  // A move seen in the actor's update ends it too.
  update.chunk = here;
  CHECK(conn.sendActorUpdate(update).ok());
  CHECK_EQ(server.recvOne()[0], 128u);
  conn.poll();
  CHECK_EQ(presence.ended.size(), 3u);
  CHECK_EQ(static_cast<int>(presence.ended[2].second), static_cast<int>(NotPresentEnd::Moved));

  // The keepalives stop: 30 seconds later the report ends by itself.
  clock.mono += 2000;
  refuse(conn, server, keepalive(conn, server, here, actor));
  CHECK_EQ(presence.made.size(), 4u);
  clock.mono += kNotPresentIdleMs - 1;
  conn.poll();
  CHECK_EQ(presence.ended.size(), 3u);
  clock.mono += 1;
  conn.poll();
  CHECK_EQ(presence.ended.size(), 4u);
  CHECK_EQ(static_cast<int>(presence.ended[3].second), static_cast<int>(NotPresentEnd::Stopped));

  // Two actors, two reports, each its own.
  const core::ActorUuid other = uuid('q');
  clock.mono += 2000;
  refuse(conn, server, keepalive(conn, server, here, actor));
  refuse(conn, server, keepalive(conn, server, here, other));
  CHECK_EQ(presence.made.size(), 6u);
  CHECK(presence.made[5].uuid == other);
  CHECK(conn.notPresent(actor).has_value() && conn.notPresent(other).has_value());

  // The actor's own update comes back from the server, signed: it is present there. (The
  // helper's notification is actor 'b' at chunk (1, 2, 3).)
  const core::ActorUuid echoed = uuid('b');
  const wire::ChunkCoord echoedAt{1, 2, 3};
  refuse(conn, server, keepalive(conn, server, echoedAt, echoed));
  CHECK_EQ(presence.made.size(), 7u);
  deliver(conn, server, signedNote(wire::MessageType::ActorUpdateNotification));
  CHECK_EQ(presence.ended.size(), 5u);
  CHECK(presence.ended[4].first.uuid == echoed);
  CHECK_EQ(static_cast<int>(presence.ended[4].second), static_cast<int>(NotPresentEnd::Accepted));
  // Another actor's update, and this actor's from another chunk, end nothing.
  CHECK(conn.notPresent(actor).has_value() && conn.notPresent(other).has_value());

  // What is not a keepalive's refusal: a sequence number no keepalive left under, a
  // keepalive's number once another message has taken it, and an answer that comes after
  // the time a refusal takes.
  const std::size_t errorsBefore = seen.errors.size();
  const std::uint64_t refusedBefore = conn.stats().keepalivesRefused;
  refuse(conn, server, 200);
  CHECK(!seen.errors.back().keepalive);
  const core::ActorUuid third = uuid('r');
  const std::uint8_t taken = keepalive(conn, server, here, third);
  for (int i = 0; i < 256; ++i) {
    CHECK(conn.sendText(update).ok());
    CHECK_EQ(server.recvOne()[0], 136u);
  }
  refuse(conn, server, taken);  // the text that took the number is what this answers
  CHECK(!seen.errors.back().keepalive);
  const std::uint8_t late = keepalive(conn, server, here, third);
  clock.mono += kKeepaliveAnswerWaitMs + 1;
  refuse(conn, server, late);
  CHECK(!seen.errors.back().keepalive);
  CHECK_EQ(seen.errors.size(), errorsBefore + 3);
  CHECK_EQ(conn.stats().keepalivesRefused, refusedBefore);
  CHECK(!conn.notPresent(third).has_value());

  // Another code for a keepalive is a keepalive's refusal, and no report.
  refuse(conn, server, keepalive(conn, server, here, third), 18);  // INVALID_APP_ID
  CHECK(seen.errors.back().keepalive);
  CHECK_EQ(static_cast<int>(seen.errors.back().code), 18);
  CHECK(!conn.notPresent(third).has_value());
  CHECK_EQ(conn.stats().keepalivesRefused, refusedBefore);

  // A reassignment: a keepalive sent to the server the connection left gets no answer,
  // and that silence is not acceptance.
  clock.mono += 2000;
  refuse(conn, server, keepalive(conn, server, here, third));
  CHECK(conn.notPresent(third).has_value());
  clock.mono += 2000;
  keepalive(conn, server, here, third);
  conn.requestReassignment();
  conn.pump(0);
  CHECK_EQ(provider->assigns, 2);
  conn.pump(0);
  clock.mono += kKeepaliveAnswerWaitMs;
  conn.poll();
  CHECK(conn.notPresent(third).has_value());
  // The first keepalive on the new assignment is the one to wait on.
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));
  const std::size_t endedBefore = presence.ended.size();
  keepalive(conn, server, here, third);
  clock.mono += kKeepaliveAnswerWaitMs;
  conn.poll();
  CHECK(!conn.notPresent(third).has_value());
  bool accepted = false;
  for (std::size_t i = endedBefore; i < presence.ended.size(); ++i) {
    if (presence.ended[i].first.uuid == third) {
      accepted = presence.ended[i].second == NotPresentEnd::Accepted;
    }
  }
  CHECK(accepted);

  // A hundred forged refusals of one keepalive make one report and move nothing.
  const core::ActorUuid target = uuid('s');
  const std::size_t madeBefore = presence.made.size();
  const std::uint8_t aimed = keepalive(conn, server, here, target);
  for (int i = 0; i < 100; ++i) refuse(conn, server, aimed);
  CHECK_EQ(presence.made.size(), madeBefore + 1);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));
  CHECK_EQ(provider->assigns, 2);
  CHECK_EQ(provider->refreshes, 0);
  CHECK_EQ(conn.stats().unverifiedTokenExpired, 0u);
  CHECK_EQ(std::string(notPresentEndName(NotPresentEnd::Accepted)), "Accepted");
  CHECK_EQ(std::string(notPresentEndName(NotPresentEnd::Moved)), "Moved");
  CHECK_EQ(std::string(notPresentEndName(NotPresentEnd::Stopped)), "Stopped");
  conn.disconnect();
}

// ---------------------------------------------------------------------------
// Datagrams that arrive faster than they are verified. The flood here does not depend on how
// fast anything runs: the crypto provider the connection verifies with sends two more
// datagrams for every one it verifies. Where the loopback hands a datagram over before
// sendto() returns (Linux, as it comes), the socket is therefore never drained, by
// construction, and a receive loop that ends only on an empty socket never ends. Where it
// does not (macOS, or a Linux loopback with a delay on it), the same flood is the best there
// is: the checks that need "never drained" are made only when loopbackIsImmediate() found the
// first kind, and the others are made everywhere.
// ---------------------------------------------------------------------------

/// Whether this machine's loopback hands a datagram to its receiver before sendto() returns:
/// 50 datagrams, each read without waiting right after its send.
bool loopbackIsImmediate() {
  const int receiver = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  const int sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  CHECK(receiver >= 0 && sender >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(::bind(receiver, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  socklen_t len = sizeof(addr);
  CHECK(::getsockname(receiver, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  bool immediate = true;
  for (int i = 0; i < 50 && immediate; ++i) {
    const std::uint8_t byte = 1;
    std::uint8_t got = 0;
    immediate = ::sendto(sender, &byte, 1, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 1 &&
                ::recv(receiver, &got, 1, MSG_DONTWAIT) == 1;
  }
  ::close(sender);
  ::close(receiver);
  return immediate;
}

/// OpenSSL, with a hook before every HMAC. It makes no reusable MAC, so every verification
/// comes through hmacSha256.
struct HookedCrypto final : core::ICrypto {
  std::function<void()> beforeHmac;
  bool hmacSha256(Bytes k, Bytes message, std::uint8_t* out) const override {
    if (beforeHmac) beforeHmac();
    return crypto().hmacSha256(k, message, out);
  }
  bool sha256(Bytes message, std::uint8_t* out) const override { return crypto().sha256(message, out); }
  bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::size_t len) const override {
    return crypto().constantTimeEquals(a, b, len);
  }
  bool randomBytes(std::uint8_t* out, std::size_t len) const override {
    return crypto().randomBytes(out, len);
  }
};

/// Fails the test, instead of hanging it, when `what` has not finished in a minute.
struct Deadline {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  std::thread watcher;
  explicit Deadline(const char* what) {
    watcher = std::thread([this, what] {
      std::unique_lock lock(mu);
      if (!cv.wait_for(lock, std::chrono::seconds(60), [this] { return done; })) {
        std::fprintf(stderr, "FAIL: %s did not return within 60 s\n", what);
        std::_Exit(1);
      }
    });
  }
  ~Deadline() {
    {
      std::lock_guard lock(mu);
      done = true;
    }
    cv.notify_all();
    watcher.join();
  }
};

// One receive pass ends after kReceivePassDatagrams, whatever is still queued (a pass used
// to end only on a short batch, so this pump() did not return).
void runAReceivePassEnds(bool immediate) {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<Provider>(server.port);
  HookedCrypto hooked;
  const auto note = signedNote(wire::MessageType::ActorUpdateNotification);
  std::atomic<bool> feeding{false};
  std::atomic<std::uint64_t> fed{0};
  hooked.beforeHmac = [&] {
    if (!feeding.load(std::memory_order_acquire)) return;  // the connection's own signing
    for (int i = 0; i < 2; ++i)
      if (server.send(note)) fed.fetch_add(1, std::memory_order_relaxed);
  };
  Connection conn(playerConfig(), provider, hooked);
  open(conn, server);
  feeding.store(true, std::memory_order_release);
  for (std::size_t i = 0; i < 2 * kReceiveBatchDatagrams; ++i)
    if (server.send(note)) fed.fetch_add(1, std::memory_order_relaxed);

  std::size_t read = 0;
  {
    Deadline deadline("pump() under a flood");
    read = conn.pump(1000);
  }
  CHECK(read > 0);
  CHECK(read <= kReceivePassDatagrams);
  CHECK_EQ(conn.stats().datagramsReceived, static_cast<std::uint64_t>(read));
  if (immediate) {
    // The pass read all it may, and ended with datagrams still to read: every batch was full.
    CHECK_EQ(read, kReceivePassDatagrams);
    CHECK(fed.load() > read);
    // The next pass goes on from there.
    Deadline deadline("the second pump() under a flood");
    CHECK_EQ(conn.pump(0), kReceivePassDatagrams);
  }
  feeding.store(false, std::memory_order_release);
  conn.disconnect();
}

struct DisconnectLogger final : core::ILogger {
  std::function<void()> onDisconnect;
  bool enabled(core::LogLevel) const override { return true; }
  void log(core::LogLevel, std::string_view message) const override {
    if (message.rfind("disconnect:", 0) == 0 && onDisconnect) onDisconnect();
  }
};

// disconnect() on a connection with its own network thread, while the flood goes on. The
// network thread is held in the middle of a batch until disconnect() has asked it to stop
// (the connection's own log line says when), so what it verifies after that is counted
// exactly: the rest of the batch in hand at most, and nothing of what is still arriving.
void runDisconnectReturnsUnderAFlood(bool immediate) {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<Provider>(server.port);
  HookedCrypto hooked;
  DisconnectLogger logger;
  const auto note = signedNote(wire::MessageType::ActorUpdateNotification);
  constexpr std::uint64_t kHoldAt = 200;  // the eighth datagram of a batch
  std::atomic<bool> feeding{false};
  std::atomic<std::uint64_t> verified{0};
  std::atomic<std::uint64_t> fed{0};
  std::mutex mu;
  std::condition_variable cv;
  bool held = false;
  bool released = false;
  hooked.beforeHmac = [&] {
    if (!feeding.load(std::memory_order_acquire)) return;  // the connection's own signing
    const std::uint64_t n = verified.fetch_add(1, std::memory_order_relaxed) + 1;
    for (int i = 0; i < 2; ++i)
      if (server.send(note)) fed.fetch_add(1, std::memory_order_relaxed);
    if (n != kHoldAt) return;
    std::unique_lock lock(mu);
    held = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  };
  logger.onDisconnect = [&] {
    std::lock_guard lock(mu);
    released = true;
    cv.notify_all();
  };

  Config cfg = playerConfig();
  cfg.manualPump = false;
  Connection conn(cfg, provider, hooked, core::systemClock(), logger);
  CHECK(conn.connect().ok());
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);
  feeding.store(true, std::memory_order_release);

  Deadline deadline("disconnect() under a flood");
  for (std::size_t i = 0; i < 2 * kReceiveBatchDatagrams; ++i)
    if (server.send(note)) fed.fetch_add(1, std::memory_order_relaxed);
  {
    // Until the network thread is held. On Linux the flood feeds itself; where a loopback
    // send can be refused or lost, this keeps it fed.
    std::unique_lock lock(mu);
    while (!cv.wait_for(lock, std::chrono::milliseconds(5), [&] { return held; })) {
      if (server.send(note)) fed.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // The network thread is inside its 200th verification and goes no further until
  // disconnect() says it has asked it to stop.
  CHECK_EQ(verified.load(), kHoldAt);
  const auto started = std::chrono::steady_clock::now();
  conn.disconnect();
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
  const std::uint64_t after = verified.load() - kHoldAt;
  std::printf("disconnect() under a flood: %lld ms, %llu datagrams verified after it was called\n",
              static_cast<long long>(took), static_cast<unsigned long long>(after));
  CHECK(after < kReceiveBatchDatagrams);
  // The flood had not stopped: more was sent than was ever read.
  if (immediate) CHECK(fed.load() > conn.stats().datagramsReceived);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Closed));
}

}  // namespace

int main() {
  runWhatACorrectServerSendsArrives();
  runWhatIsDropped();
  runWhatIsStillTaken();
  runTokenExpired();
  runARefusedKeepaliveIsReportedOnce();
  const bool immediate = loopbackIsImmediate();
  std::printf("loopback hands a datagram over before sendto() returns: %s\n", immediate ? "yes" : "no");
  runAReceivePassEnds(immediate);
  runDisconnectReturnsUnderAFlood(immediate);
  std::puts("player_connection_checks_test OK");
  return 0;
}
