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
//    for a fresh token, at most once in kTokenCheckIntervalMs, and its answer decides.
//
// Every wait here is on a count (what was delivered, what was counted as dropped), never on
// an amount of time passing.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
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

  /// Best effort, as a datagram is: a full receive buffer drops it. Returns whether the
  /// kernel took it.
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

template <class Done>
void pumpUntil(Connection& conn, Done done) {
  for (int i = 0; i < 500 && !done(); ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK(done());
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

  for (const wire::MessageType type : kNotificationTypes) {
    const auto note = signedNote(type);
    CHECK(server.send(note));                                   // alone, its own HMAC
    CHECK(server.send(plainBundle({note, note})));              // a plain bundle's members
    CHECK(server.send(signedBundle({bare(note), bare(note)})));  // a signed bundle's members
    CHECK(server.send(signedBundle({bare(note)})));              // a signed bundle of one
  }
  pumpUntil(conn, [&] { return seen.spatial == kTypeCount * 6; });
  for (const wire::MessageType type : kNotificationTypes)
    CHECK_EQ(seen.spatialByType[static_cast<int>(type)], 6);

  // A video fragment too large to be a bundle's member travels alone with its own HMAC.
  CHECK(server.send(signedNote(wire::MessageType::ClientVideoNotification, kApp, 1100)));
  pumpUntil(conn, [&] { return seen.spatial == kTypeCount * 6 + 1; });

  // Channel notifications: plain to a session that has not advertised, in a signed bundle
  // to one that has.
  for (const std::uint8_t type : {std::uint8_t{18}, std::uint8_t{36}}) {
    CHECK(server.send(channelNote(type)));
    CHECK(server.send(plainBundle({channelNote(type)})));
    CHECK(server.send(signedBundle({channelNote(type)})));
  }
  pumpUntil(conn, [&] { return seen.channelMessages == 3 && seen.channelAudio == 3; });

  // A refusal: alone and unsigned, as a player's session is sent every one today. One
  // inside a signed bundle is marked verified.
  const std::vector<std::uint8_t> unauthorized = {3, 200, 7};
  CHECK(server.send(unauthorized));
  CHECK(server.send(plainBundle({unauthorized, signedNote(wire::MessageType::ClientTextNotification)})));
  CHECK(server.send(signedBundle({unauthorized})));
  pumpUntil(conn, [&] { return seen.errors.size() == 3 && seen.spatial == kTypeCount * 6 + 2; });
  int verified = 0;
  for (const GenericError& e : seen.errors) {
    CHECK_EQ(static_cast<int>(e.code), 7);
    CHECK_EQ(e.sequence, 200u);
    if (e.verified) ++verified;
  }
  CHECK_EQ(verified, 1);

  const auto s = conn.stats();
  CHECK_EQ(s.wrongApp, 0u);
  CHECK_EQ(s.unsignedNotifications, 0u);
  CHECK_EQ(s.hmacFailures, 0u);
  CHECK_EQ(s.malformed, 0u);
  CHECK_EQ(s.unverifiedTokenExpired, 0u);
  CHECK_EQ(s.reconnects, 0u);
  CHECK_EQ(s.messagesReceived, static_cast<std::uint64_t>(kTypeCount * 6 + 2 + 6 + 3));
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

  // Nothing signed it: alone, and beside a signed one in a plain bundle.
  CHECK(server.send(bare(mine)));
  pumpUntil(conn, [&] { return conn.stats().unsignedNotifications == 1; });
  CHECK(server.send(plainBundle({bare(mine), mine, bare(mine)})));
  pumpUntil(conn, [&] { return conn.stats().unsignedNotifications == 3 && seen.spatial == 1; });

  // Each type, unsigned and alone: none is taken.
  for (const wire::MessageType type : kNotificationTypes) CHECK(server.send(bare(signedNote(type))));
  pumpUntil(conn, [&] {
    return conn.stats().unsignedNotifications == static_cast<std::uint64_t>(3 + kTypeCount);
  });
  CHECK_EQ(seen.spatial, 1);

  // Another app's, with an HMAC of its own that holds: alone, and in a plain bundle.
  CHECK(server.send(others));
  pumpUntil(conn, [&] { return conn.stats().wrongApp == 1; });
  CHECK(server.send(plainBundle({others, mine})));
  pumpUntil(conn, [&] { return conn.stats().wrongApp == 2 && seen.spatial == 2; });

  // Another app's inside a signed bundle that verifies, beside this app's.
  CHECK(server.send(signedBundle({bare(others), bare(mine), bare(others)})));
  pumpUntil(conn, [&] { return conn.stats().wrongApp == 4 && seen.spatial == 3; });

  // Each type of another app, signed: none is taken.
  for (const wire::MessageType type : kNotificationTypes) CHECK(server.send(signedNote(type, kOtherApp)));
  pumpUntil(conn, [&] { return conn.stats().wrongApp == static_cast<std::uint64_t>(4 + kTypeCount); });
  CHECK_EQ(seen.spatial, 3);

  // Another app's that nothing signed is counted once, as unsigned: the signature is read
  // first, and without one the app it names is nobody's word.
  CHECK(server.send(bare(others)));
  pumpUntil(conn, [&] {
    return conn.stats().unsignedNotifications == static_cast<std::uint64_t>(4 + kTypeCount);
  });
  CHECK_EQ(conn.stats().wrongApp, static_cast<std::uint64_t>(4 + kTypeCount));

  // A bad HMAC is what it always was.
  auto tampered = mine;
  tampered[wire::offsets::kPayload] ^= 0xff;
  CHECK(server.send(tampered));
  pumpUntil(conn, [&] { return conn.stats().hmacFailures == 1; });

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
  const std::uint64_t wrongBefore = conn.stats().wrongApp;
  const std::uint64_t unsignedBefore = conn.stats().unsignedNotifications;
  CHECK(server.send(signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp, 12, seq.value())));
  CHECK(server.send(bare(signedNote(wire::MessageType::ActorUpdateNotification, kApp, 12, seq.value()))));
  pumpUntil(conn, [&] {
    return conn.stats().wrongApp == wrongBefore + 1 &&
           conn.stats().unsignedNotifications == unsignedBefore + 1;
  });
  // Both were read and dropped before this wait begins, so it has nothing to find.
  CHECK(!conn.waitForSequence(seq.value(), send.uuid, 1).acknowledged);
  // The real echo does answer it.
  CHECK(server.send(signedNote(wire::MessageType::ActorUpdateNotification, kApp, 12, seq.value())));
  CHECK(conn.waitForSequence(seq.value(), send.uuid, 5000).acknowledged);

  const auto s = conn.stats();
  CHECK_EQ(seen.spatial, 4);  // the three of this app from above, and the echo
  CHECK_EQ(s.messagesReceived, 4u);
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
    CHECK(server.send(bare(mine)));
    CHECK(server.send(plainBundle({bare(mine), mine})));
    CHECK(server.send(signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp)));
    pumpUntil(conn, [&] { return seen.spatial == 3 && conn.stats().wrongApp == 1; });
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
    CHECK(server.send(signedNote(wire::MessageType::ActorUpdateNotification, kApp)));
    CHECK(server.send(signedNote(wire::MessageType::ActorUpdateNotification, kOtherApp)));
    pumpUntil(conn, [&] { return seen.spatial == 2; });
    CHECK_EQ(conn.stats().wrongApp, 0u);
    // It still takes nothing unsigned.
    CHECK(server.send(bare(signedNote(wire::MessageType::ActorUpdateNotification, kApp))));
    pumpUntil(conn, [&] { return conn.stats().unsignedNotifications == 1; });
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
    CHECK(server.send(signedNote(wire::MessageType::ServerEventNotification, 0)));
    CHECK(server.send(signedBundle({bare(signedNote(wire::MessageType::ServerEventNotification, 0))})));
    pumpUntil(conn, [&] { return seen.spatial == 2; });
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
    CHECK(server.send(expired));
    CHECK(server.send(plainBundle({expired})));
  }
  pumpUntil(conn, [&] { return seen.errors.size() == 100; });
  for (int i = 0; i < 5; ++i) {
    conn.pump(0);
    conn.poll();
  }
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
  CHECK(server.send(expired));
  pumpUntil(conn, [&] { return seen.errors.size() == 101; });
  conn.pump(0);
  CHECK_EQ(provider->refreshes, 1);

  // At the interval: a second question. This time the provider cannot say: nothing changes.
  clock.mono += 1;
  provider->refreshFails = true;
  CHECK(server.send(expired));
  pumpUntil(conn, [&] { return seen.errors.size() == 102 && provider->refreshes == 2; });
  conn.pump(0);
  CHECK_EQ(provider->assigns, 1);
  CHECK_EQ(conn.stats().reconnects, 0u);
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));

  // The provider says the fresh token is not known to this server: that answer, which is
  // the Application API's, reassigns, as it does after a refresh that was due.
  clock.mono += kTokenCheckIntervalMs;
  provider->refreshFails = false;
  provider->authorizedOnCurrent = false;
  CHECK(server.send(expired));
  pumpUntil(conn, [&] { return provider->assigns == 2; });
  CHECK_EQ(provider->refreshes, 3);
  CHECK_EQ(conn.stats().reconnects, 1u);
  CHECK_EQ(seen.errors.size(), 103u);
  provider->authorizedOnCurrent = true;

  // Verified: it reassigns at once, and asks the provider for no token.
  conn.pump(0);
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);
  CHECK(server.send(signedBundle({expired})));
  pumpUntil(conn, [&] { return provider->assigns == 3; });
  CHECK_EQ(provider->refreshes, 3);
  CHECK_EQ(seen.errors.size(), 104u);
  CHECK(seen.errors.back().verified);
  CHECK_EQ(conn.stats().reconnects, 2u);
  CHECK_EQ(conn.stats().unverifiedTokenExpired, 103u);

  // A signed bundle whose HMAC does not hold is dropped whole: its TOKEN_EXPIRED reaches
  // nobody and moves nothing.
  conn.pump(0);
  CHECK(conn.sendHeartbeat({0, 0, 0}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);
  auto forged = signedBundle({expired});
  forged.back() ^= 0x01;
  CHECK(server.send(forged));
  pumpUntil(conn, [&] { return conn.stats().hmacFailures == 1; });
  conn.pump(0);
  conn.poll();
  CHECK_EQ(seen.errors.size(), 104u);
  CHECK_EQ(provider->assigns, 3);

  // An unverified refusal still settles the awaited send it names.
  SpatialSend send;
  send.chunk = {1, 2, 3};
  send.uuid = uuid('a');
  const auto seq = conn.sendText(send);
  CHECK(seq.ok());
  CHECK_EQ(server.recvOne()[0], 136u);
  CHECK(server.send({3, seq.value(), 32}));
  const auto outcome = conn.waitForSequence(seq.value(), send.uuid, 5000);
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
    CHECK(other.send(expired));
    pumpUntil(quiet, [&] { return heard.errors.size() == 1; });
    quiet.pump(0);
    CHECK_EQ(own->refreshes, 0);
    CHECK_EQ(own->assigns, 1);
    CHECK_EQ(quiet.stats().unverifiedTokenExpired, 1u);
    CHECK_EQ(quiet.stats().reconnects, 0u);
    quiet.disconnect();
  }
}

}  // namespace

int main() {
  runWhatACorrectServerSendsArrives();
  runWhatIsDropped();
  runWhatIsStillTaken();
  runTokenExpired();
  std::puts("player_connection_checks_test OK");
  return 0;
}
