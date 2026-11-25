// Offline FleetSession test, in replication_test's manner: a fake in-process UDP "server" on
// 127.0.0.1 receives what the session sends, checks it byte for byte, and answers with the
// fleet session's server-to-client messages. Manual-pump mode on a clock the test moves,
// except the last case, which runs the owned network thread.
//
// The golden bytes are wire_test's, computed independently (Python hmac/hashlib) from
// DESIGN-2026-11-fleet-session section 6.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// FLEET_CARRY (49) for the player above at kEpochMs, sequence 0.
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

struct FakeServer {
  int fd = -1;
  int port = 0;
  sockaddr_in lastClient{};
  socklen_t lastClientLen = 0;

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

  /// The next datagram, or empty when none arrives in time.
  std::vector<std::uint8_t> tryRecv(int timeoutMs) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) return {};
    lastClient = from;
    lastClientLen = fromLen;
    return std::vector<std::uint8_t>(buf, buf + n);
  }

  std::vector<std::uint8_t> recvOne(int timeoutMs = 2000) {
    auto got = tryRecv(timeoutMs);
    CHECK(!got.empty());
    return got;
  }

  void expectNothing() { CHECK(tryRecv(60).empty()); }

  void send(const std::vector<std::uint8_t>& data) {
    CHECK(lastClientLen > 0);
    CHECK(::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&lastClient),
                   lastClientLen) == static_cast<ssize_t>(data.size()));
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
  // Each send is read straight off the wire, and sequence numbers are part of the golden
  // bytes: one message a datagram and no capabilities in between.
  cfg.transport.bundleSends = false;
  cfg.transport.advertiseCapabilities = false;
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
                                       const wire::Token64& key) {
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = kAppId;
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

/// The same notification as a signed bundle's member: containsAuth 0 and no HMAC of its own.
std::vector<std::uint8_t> withoutOwnHmac(std::vector<std::uint8_t> note) {
  note[wire::offsets::kContainsAuth] = 0;
  const std::size_t hmacAt = note.size() - wire::kTailWithHmac;
  note.erase(note.begin() + static_cast<std::ptrdiff_t>(hmacAt),
             note.begin() + static_cast<std::ptrdiff_t>(hmacAt + wire::kHmacTagSize));
  return note;
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

/// A few passes with nothing expected, so anything wrongly queued would be dispatched.
void settle(FleetSession& fleet) {
  for (int i = 0; i < 3; ++i) {
    fleet.pump(20);
    fleet.poll();
  }
}

void openConnected(FleetSession& fleet) {
  CHECK(fleet.open().ok());
  fleet.pump(0);
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Connected));
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
  server.send(carryResultFrame(id, user, 0));
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

// ---------------------------------------------------------------------------
// Carry, send for the player, release: the golden bytes.
// ---------------------------------------------------------------------------

void runCarrySendRelease() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));

  const std::uint8_t pose[] = {0xde, 0xad, 0xbe, 0xef};
  const SpatialSend update = goldenUpdate(pose);

  // Nothing before open(); a malformed token and the session's own id are refused.
  CHECK_EQ(static_cast<int>(fleet.carry({kPlayerToken, kPlayerId}).code),
           static_cast<int>(Errc::NotConnected));
  openConnected(fleet);
  CHECK_EQ(static_cast<int>(fleet.carry({"short", kPlayerId}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK_EQ(static_cast<int>(fleet.carry({kPlayerToken, kFleetId}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK(fleet.players().empty());

  // A player the session does not carry: refused here, nothing on the wire.
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(kPlayerId, update).error()),
           static_cast<int>(Errc::NotCarried));
  CHECK_EQ(std::string(errcName(Errc::NotCarried)), std::string("NotCarried"));
  CHECK_EQ(fleet.stats().messagesSent, 0u);

  // FLEET_CARRY: the golden bytes, and a proof an HMAC computed here agrees with.
  std::vector<FleetCarryResult> viaCallback;
  CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                viaCallback.push_back(r);
              }).ok());
  auto carry = server.recvOne();
  CHECK_EQ(toHex(carry), std::string(kGoldenFleetCarry));
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

  // FLEET_CARRY_RESULT, carried: once to the callback, then once to the handler.
  server.send(carryResultFrame(kPlayerId, 77, 0));
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
  server.send(carryResultFrame(kPlayerId, 77, 0));
  server.send(carryResultFrame(kPlayerId, 0, 1));
  settle(fleet);
  CHECK_EQ(viaCallback.size(), 1u);
  CHECK_EQ(seen.results.size(), 1u);
  CHECK(fleet.carries(kPlayerId));

  // The player's actor update: the golden bytes. The tail carries the PLAYER's id, and
  // the HMAC verifies with the FLEET token and not with the player's.
  auto seq = fleet.sendActorUpdate(kPlayerId, update);
  CHECK(seq.ok());
  CHECK_EQ(seq.value(), 1u);
  auto actor = server.recvOne();
  CHECK_EQ(toHex(actor), std::string(kGoldenCarriedActorUpdate));
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

  // FLEET_RELEASE: the golden bytes, and the player is out of the table at once.
  CHECK(fleet.release(kPlayerId).ok());
  auto release = server.recvOne();
  CHECK_EQ(toHex(release), std::string(kGoldenFleetRelease));
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

  // close() empties the table and ends carrying.
  carryAs(fleet, server, kPlayerId, 77, kPlayerToken);
  fleet.close();
  CHECK_EQ(static_cast<int>(fleet.state()), static_cast<int>(ConnState::Closed));
  CHECK(fleet.players().empty());
  CHECK_EQ(static_cast<int>(fleet.carry({kPlayerToken, kPlayerId}).code),
           static_cast<int>(Errc::NotConnected));
  CHECK_EQ(static_cast<int>(fleet.sendActorUpdate(kPlayerId, update).error()),
           static_cast<int>(Errc::NotCarried));
}

// ---------------------------------------------------------------------------
// Every send Connection offers, for a carried player: the player's id in the tail, the
// fleet token's HMAC. The session's own messages carry the fleet id.
// ---------------------------------------------------------------------------

void runEverySendForAPlayer() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  openConnected(fleet);
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
  // The player's speaker lists: one kind a message, the player's id in the tail.
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

  // The session's own heartbeat and capabilities: no actor, chunk (0,0,0), the FLEET id.
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
  {
    auto got = server.recvOne();
    CHECK_EQ(got[0], 29u);
    auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
    CHECK(v.ok());
    CHECK_EQ(v->epochMillisOrTokenId, kFleetId);
    CHECK(wire::verifyLongSpatial(crypto(), Bytes(got.data(), got.size()), fleetKey()).ok());
  }
}

// ---------------------------------------------------------------------------
// A carry is sent again every 250 ms with a fresh time and proof, and ends TimedOut 5 s
// after its first send.
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
  openConnected(fleet);

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

  // At 250 ms: again, with the time of the resend and the proof for that time.
  clock.advance(1);
  fleet.pump(0);
  const Carry second = readCarry(server.recvOne(), kPlayerToken);
  CHECK_EQ(second.player, kPlayerId);
  CHECK_EQ(second.epochMs, kEpochMs + 250);
  CHECK(second.proof != first.proof);
  // One pass sends it once.
  fleet.pump(0);
  server.expectNothing();

  // Every 250 ms up to 4,750 ms: 20 carries in all, and no result reported yet.
  for (int step = 2; step <= 19; ++step) {
    clock.advance(250);
    fleet.pump(0);
    fleet.poll();
    const Carry again = readCarry(server.recvOne(), kPlayerToken);
    CHECK_EQ(again.epochMs, kEpochMs + 250 * step);
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
  server.send(carryResultFrame(kPlayerId, 77, 0));
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
  server.send(carryResultFrame(kPlayerId, 77, 0));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  clock.advance(1000);
  fleet.pump(0);
  fleet.poll();
  server.expectNothing();
  CHECK(fleet.carries(kPlayerId));
  CHECK_EQ(seen.results.size(), 2u);
  CHECK(seen.results[1].carried());

  // A carry made before the session is connected leaves once it is.
  FakeServer later;
  later.start();
  FleetSession waiting(configFor(later), crypto(), clock);
  CHECK(waiting.open().ok());
  CHECK_EQ(static_cast<int>(waiting.state()), static_cast<int>(ConnState::Connecting));
  CHECK(waiting.carry({kPlayerToken, kPlayerId}).ok());
  CHECK_EQ(waiting.stats().messagesSent, 0u);
  waiting.pump(0);
  CHECK_EQ(readCarry(later.recvOne(), kPlayerToken).player, kPlayerId);
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
  openConnected(fleet);

  struct Case {
    std::uint8_t wire;
    FleetCarryStatus status;
    const char* name;
  };
  const Case cases[] = {
      {1, FleetCarryStatus::NoLiveToken, "NoLiveToken"},
      {2, FleetCarryStatus::OtherOrganizationOrApp, "OtherOrganizationOrApp"},
      {3, FleetCarryStatus::ProofFailed, "ProofFailed"},
      {4, FleetCarryStatus::SessionFull, "SessionFull"},
      {9, FleetCarryStatus::Refused, "Refused"},  // a status this build does not know
  };
  for (const Case& c : cases) {
    std::vector<FleetCarryResult> viaCallback;
    CHECK(fleet.carry({kPlayerToken, kPlayerId}, [&](const FleetCarryResult& r) {
                  viaCallback.push_back(r);
                }).ok());
    readCarry(server.recvOne(), kPlayerToken);
    const std::size_t before = seen.results.size();
    // A refusal's user id is 0 on the wire; one that is not must not be believed.
    server.send(carryResultFrame(kPlayerId, c.wire == 2 ? 77 : 0, c.wire));
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

  // Status 0, with the user id.
  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  readCarry(server.recvOne(), kPlayerToken);
  server.send(carryResultFrame(kPlayerId, 123456789012LL, 0));
  pumpUntil(fleet, [&] { return fleet.carries(kPlayerId); });
  CHECK(seen.results.back().carried());
  CHECK_EQ(seen.results.back().userId, 123456789012LL);
  CHECK_EQ(std::string(fleetCarryStatusName(seen.results.back().status)), std::string("Carried"));

  // A result for a player the session never carried, and a truncated one, change nothing.
  const std::size_t before = seen.results.size();
  server.send(carryResultFrame(31337, 5, 0));
  auto truncated = carryResultFrame(kPlayerId, 77, 0);
  truncated.pop_back();
  server.send(truncated);
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 1; });
  settle(fleet);
  CHECK_EQ(seen.results.size(), before);
  CHECK(!fleet.player(31337).has_value());
  CHECK_EQ(fleet.stats().malformed, 1u);
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
  openConnected(fleet);

  const std::int64_t a = 101, b = 102, c = 103;
  carryAs(fleet, server, a, 1001, tokenFor(a));
  carryAs(fleet, server, b, 1002, tokenFor(b));
  carryAs(fleet, server, c, 1003, tokenFor(c));
  CHECK_EQ(fleet.carriedCount(), 3u);

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
    server.send(releasedFrame(r.player, r.wire));
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

  // A release for a player the table does not hold is still reported, without a user.
  server.send(releasedFrame(999, 1));
  pumpUntil(fleet, [&] { return seen.released.size() == 4; });
  CHECK_EQ(seen.released.size(), 4u);
  CHECK_EQ(seen.released.back().playerGameTokenId, 999);
  CHECK_EQ(seen.released.back().userId, 0);

  // A crossing back: carried again, then released while a carry with a refreshed token
  // waits. The waiting carry keeps its row and still reports its own result.
  carryAs(fleet, server, a, 1001, tokenFor(a));
  CHECK(fleet.carry({tokenFor(a), a}).ok());
  readCarry(server.recvOne(), tokenFor(a));
  server.send(releasedFrame(a, 2));
  pumpUntil(fleet, [&] { return seen.released.size() == 5; });
  CHECK(!fleet.carries(a));
  CHECK(fleet.player(a).has_value());
  CHECK(fleet.player(a)->pending);
  const std::size_t results = seen.results.size();
  server.send(carryResultFrame(a, 0, 1));
  pumpUntil(fleet, [&] { return seen.results.size() > results; });
  CHECK_EQ(static_cast<int>(seen.results.back().status), static_cast<int>(FleetCarryStatus::NoLiveToken));
  CHECK(!fleet.player(a).has_value());

  // A truncated release is malformed.
  auto truncated = releasedFrame(b, 1);
  truncated.pop_back();
  server.send(truncated);
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 1; });
  CHECK_EQ(fleet.stats().malformed, 1u);
  CHECK_EQ(seen.released.size(), 5u);
}

// ---------------------------------------------------------------------------
// A refreshed player token: the new carry replaces the same user's older entry once its
// result arrives.
// ---------------------------------------------------------------------------

void runRefreshedTokenReplacesEntry() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet);

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
  server.send(carryResultFrame(newId, user, 0));
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
    CHECK_EQ(le::readI64(got.data() + got.size() - wire::kTailNoHmac), newId);
  }
  CHECK(fleet.sendActorUpdate(newId, p).ok());
  {
    auto got = server.recvOne();
    CHECK_EQ(got[0], 128u);
    CHECK_EQ(le::readI64(got.data() + got.size() - wire::kTailNoHmac), newId);
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
  server.send(carryResultFrame(newId, user, 0));
  pumpUntil(fleet, [&] { return !fleet.player(newId)->pending; });
  CHECK(fleet.carries(newId));
  CHECK_EQ(fleet.carriedCount(), 2u);
}

// ---------------------------------------------------------------------------
// Receiving: plain state once with no recipients; FLEET_DELIVERY with its recipients.
// ---------------------------------------------------------------------------

void runDeliveryAndPlainState() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);

  struct Got {
    std::uint8_t type = 0;
    std::vector<std::int64_t> recipients;
    std::vector<std::uint8_t> payload;
    std::int64_t channelId = 0;
  };
  std::vector<Got> any, actorUpdates, voxels, texts, audio, video, direct, events, channelMessages, channelAudio;
  auto spatial = [](std::vector<Got>& into) {
    return [&into](const SpatialNotification& n, FleetRecipients r) {
      into.push_back({static_cast<std::uint8_t>(n.type), {r.begin(), r.end()},
                      {n.payload.begin(), n.payload.end()}, 0});
    };
  };
  auto channel = [](std::vector<Got>& into, std::uint8_t type) {
    return [&into, type](const ChannelNotification& n, FleetRecipients r) {
      into.push_back({type, {r.begin(), r.end()}, {n.payload.begin(), n.payload.end()}, n.channelId});
    };
  };
  FleetHandlers h;
  h.any = spatial(any);
  h.actorUpdate = spatial(actorUpdates);
  h.text = spatial(texts);
  h.audio = spatial(audio);
  h.video = spatial(video);
  h.singleActorMessage = spatial(direct);
  h.voxelUpdate = [&](const SpatialNotification& n, const wire::VoxelPayloadView& v, FleetRecipients r) {
    voxels.push_back({static_cast<std::uint8_t>(n.type), {r.begin(), r.end()},
                      {v.state.begin(), v.state.end()}, v.voxelType});
  };
  h.clientEvent = [&](const SpatialNotification& n, const wire::EventPayloadView& e, FleetRecipients r) {
    events.push_back({static_cast<std::uint8_t>(n.type), {r.begin(), r.end()},
                      {e.state.begin(), e.state.end()}, e.eventType});
  };
  h.channelMessage = channel(channelMessages, 18);
  h.channelAudio = channel(channelAudio, 36);
  fleet.setHandlers(std::move(h));
  openConnected(fleet);

  const std::int64_t a = 301, b = 302;
  carryAs(fleet, server, a, 3001, tokenFor(a));
  carryAs(fleet, server, b, 3002, tokenFor(b));

  const std::uint8_t body[] = {'h', 'i'};
  const Bytes bytes(body, sizeof(body));

  // Plain state: dispatched once, with no recipients.
  server.send(notification(wire::MessageType::ActorUpdateNotification, bytes, 1, fleetKey()));
  pumpUntil(fleet, [&] { return !actorUpdates.empty(); });
  settle(fleet);
  CHECK_EQ(actorUpdates.size(), 1u);
  CHECK_EQ(any.size(), 1u);
  CHECK(actorUpdates[0].recipients.empty());
  CHECK(any[0].recipients.empty());
  CHECK_EQ(actorUpdates[0].payload.size(), 2u);
  // A voxel edit, in a signed bundle beside an event: once each, no recipients.
  std::uint8_t voxelPayload[wire::voxel::kFixedSize + 2];
  CHECK(wire::encodeVoxelPayload(1, 2, 3, 9, bytes, MutableBytes(voxelPayload, sizeof(voxelPayload))).ok());
  std::uint8_t eventPayload[wire::kEventTypeSize + 2];
  CHECK(wire::encodeEventPayload(0x0304, bytes, MutableBytes(eventPayload, sizeof(eventPayload))).ok());
  server.send(signedBundle(
      {withoutOwnHmac(notification(wire::MessageType::VoxelUpdateNotification,
                                   Bytes(voxelPayload, sizeof(voxelPayload)), 2, fleetKey())),
       withoutOwnHmac(notification(wire::MessageType::ClientEventNotification,
                                   Bytes(eventPayload, sizeof(eventPayload)), 3, fleetKey()))}));
  pumpUntil(fleet, [&] { return !events.empty(); });
  settle(fleet);
  CHECK_EQ(voxels.size(), 1u);
  CHECK_EQ(events.size(), 1u);
  CHECK(voxels[0].recipients.empty());
  CHECK(events[0].recipients.empty());
  CHECK_EQ(voxels[0].channelId, 9);
  CHECK_EQ(events[0].channelId, 0x0304);
  CHECK_EQ(any.size(), 3u);
  CHECK_EQ(fleet.stats().signedBundlesReceived, 1u);
  // State keyed with the player's token is not the session's: dropped.
  server.send(notification(wire::MessageType::ActorUpdateNotification, bytes, 4, playerKey()));
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 1; });
  CHECK_EQ(fleet.stats().hmacFailures, 1u);
  CHECK_EQ(actorUpdates.size(), 1u);

  // FLEET_DELIVERY: spatial text for two carried players.
  server.send(deliveryFrame({a, b}, notification(wire::MessageType::ClientTextNotification, bytes, 5, fleetKey())));
  pumpUntil(fleet, [&] { return !texts.empty(); });
  settle(fleet);
  CHECK_EQ(texts.size(), 1u);
  CHECK(texts[0].recipients == std::vector<std::int64_t>({a, b}));
  CHECK_EQ(texts[0].payload.size(), 2u);
  CHECK_EQ(texts[0].payload[0], 'h');
  CHECK_EQ(any.size(), 4u);
  CHECK(any.back().recipients == std::vector<std::int64_t>({a, b}));
  CHECK_EQ(any.back().type, 137u);
  CHECK_EQ(fleet.stats().messagesReceivedByType[53], 1u);

  // Voice for one player, video and a direct message: each through its typed handler.
  server.send(deliveryFrame({b}, notification(wire::MessageType::ClientAudioNotification, bytes, 6, fleetKey())));
  server.send(deliveryFrame({a}, notification(wire::MessageType::ClientVideoNotification, bytes, 7, fleetKey())));
  server.send(deliveryFrame({a}, notification(wire::MessageType::SingleActorMessage, bytes, 8, fleetKey())));
  pumpUntil(fleet, [&] { return !audio.empty() && !video.empty() && !direct.empty(); });
  CHECK_EQ(audio.size(), 1u);
  CHECK(audio[0].recipients == std::vector<std::int64_t>({b}));
  CHECK_EQ(video.size(), 1u);
  CHECK(video[0].recipients == std::vector<std::int64_t>({a}));
  CHECK_EQ(direct.size(), 1u);
  CHECK(direct[0].recipients == std::vector<std::int64_t>({a}));

  // 32 recipients, the most one frame lists, around a channel message.
  std::vector<std::int64_t> everyone;
  for (std::int64_t i = 0; i < 32; ++i) everyone.push_back(7000 + i);
  server.send(deliveryFrame(everyone, channelNote(18, 55, bytes)));
  pumpUntil(fleet, [&] { return !channelMessages.empty(); });
  CHECK_EQ(channelMessages.size(), 1u);
  CHECK_EQ(channelMessages[0].recipients.size(), 32u);
  CHECK(channelMessages[0].recipients == everyone);
  CHECK_EQ(channelMessages[0].channelId, 55);
  CHECK_EQ(channelMessages[0].payload.size(), 2u);
  // Channel audio, inside a signed bundle with a delivery whose notification has no HMAC of
  // its own.
  server.send(signedBundle(
      {deliveryFrame({a, b}, channelNote(36, 56, bytes)),
       deliveryFrame({b}, withoutOwnHmac(notification(wire::MessageType::ClientTextNotification, bytes, 9,
                                                      fleetKey())))}));
  pumpUntil(fleet, [&] { return !channelAudio.empty() && texts.size() == 2; });
  CHECK_EQ(channelAudio.size(), 1u);
  CHECK(channelAudio[0].recipients == std::vector<std::int64_t>({a, b}));
  CHECK_EQ(channelAudio[0].channelId, 56);
  CHECK(channelMessages.size() == 1u);
  CHECK_EQ(texts.size(), 2u);
  CHECK(texts[1].recipients == std::vector<std::int64_t>({b}));

  // Malformed counts: 0, 33, and a count longer than the frame. Nothing is dispatched.
  const auto text = notification(wire::MessageType::ClientTextNotification, bytes, 10, fleetKey());
  const std::size_t textsBefore = texts.size();
  const std::size_t anyBefore = any.size();
  server.send(deliveryFrame(0, {}, text));
  std::vector<std::int64_t> thirtyThree(33, a);
  server.send(deliveryFrame(33, thirtyThree, text));
  server.send(deliveryFrame(30, {a, b}, text));  // 30 named, 2 listed
  server.send(deliveryFrame(2, {a, b}, {}));  // the list and no notification
  // A notification that is no notification: a client-only type, and an unknown one.
  auto capabilities = notification(wire::MessageType::ClientCapabilities, bytes, 11, fleetKey());
  server.send(deliveryFrame({a}, capabilities));
  server.send(deliveryFrame({a}, {0x63, 1, 2, 3}));
  pumpUntil(fleet, [&] { return fleet.stats().malformed == 6; });
  settle(fleet);
  CHECK_EQ(fleet.stats().malformed, 6u);
  CHECK_EQ(texts.size(), textsBefore);
  CHECK_EQ(any.size(), anyBefore);

  // A delivery whose notification was signed with another key, or changed in flight.
  auto forged = deliveryFrame({a}, notification(wire::MessageType::ClientTextNotification, bytes, 12, playerKey()));
  server.send(forged);
  auto tampered = deliveryFrame({a}, text);
  tampered[2 + 8 + wire::offsets::kPayload] ^= 0xff;
  server.send(tampered);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 3; });
  settle(fleet);
  CHECK_EQ(fleet.stats().hmacFailures, 3u);
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
  other.send(carryResultFrame(a, 3001, 0));
  other.send(releasedFrame(a, 1));
  other.send(deliveryFrame({a}, text));
  other.send(deliveryFrame({a}, channelNote(18, 55, bytes)));
  for (int i = 0; i < 100 && conn.stats().datagramsReceived < 4; ++i) {
    conn.pump(20);
    conn.poll();
  }
  conn.poll();
  CHECK_EQ(conn.stats().datagramsReceived, 4u);
  CHECK_EQ(conn.stats().messagesReceived, 0u);
  CHECK_EQ(conn.stats().malformed, 0u);
  CHECK_EQ(plainEvents, 0);
}

// ---------------------------------------------------------------------------
// FleetConfig::requireSignedControl: the three fleet messages only from a signed bundle.
// ---------------------------------------------------------------------------

void runRequireSignedControl() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetConfig cfg = configFor(server);
  cfg.requireSignedControl = true;
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  int texts = 0;
  FleetHandlers h = recording(seen);
  h.text = [&](const SpatialNotification&, FleetRecipients r) {
    ++texts;
    CHECK_EQ(r.size(), 1u);
  };
  fleet.setHandlers(std::move(h));
  openConnected(fleet);

  CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok());
  readCarry(server.recvOne(), kPlayerToken);

  // Outside a signed bundle: dropped and counted, and the carry still waits.
  const std::uint8_t body[] = {'h', 'i'};
  const auto text = notification(wire::MessageType::ClientTextNotification, Bytes(body, 2), 1, fleetKey());
  server.send(carryResultFrame(kPlayerId, 77, 0));
  server.send(releasedFrame(kPlayerId, 1));
  server.send(deliveryFrame({kPlayerId}, text));
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 3; });
  settle(fleet);
  CHECK_EQ(fleet.stats().hmacFailures, 3u);
  CHECK(seen.results.empty());
  CHECK(seen.released.empty());
  CHECK_EQ(texts, 0);
  CHECK(fleet.player(kPlayerId)->pending);
  CHECK(!fleet.carries(kPlayerId));

  // A bundle signed with another key is dropped whole.
  auto wrong = signedBundle({carryResultFrame(kPlayerId, 77, 0)});
  wrong.back() ^= 0x01;
  server.send(wrong);
  pumpUntil(fleet, [&] { return fleet.stats().hmacFailures == 4; });
  CHECK(!fleet.carries(kPlayerId));

  // Inside one signed with the fleet token: taken.
  server.send(signedBundle({carryResultFrame(kPlayerId, 77, 0), deliveryFrame({kPlayerId}, text)}));
  pumpUntil(fleet, [&] { return texts == 1; });
  CHECK(fleet.carries(kPlayerId));
  CHECK_EQ(seen.results.size(), 1u);
  CHECK_EQ(texts, 1);
  server.send(signedBundle({releasedFrame(kPlayerId, 5)}));
  pumpUntil(fleet, [&] { return !seen.released.empty(); });
  CHECK(!fleet.carries(kPlayerId));
  CHECK_EQ(static_cast<int>(seen.released[0].reason), static_cast<int>(FleetReleaseReason::Idle));
}

// ---------------------------------------------------------------------------
// CLIENT_CAPABILITIES: the session's own, with bit 3 (FLEET_OWN_STATE) when asked.
// ---------------------------------------------------------------------------

std::uint32_t expectCapabilities(FakeServer& server) {
  auto got = server.recvOne();
  CHECK_EQ(got[0], 29u);
  auto v = wire::parseLongSpatial(Bytes(got.data(), got.size()));
  CHECK(v.ok());
  CHECK_EQ(v->epochMillisOrTokenId, kFleetId);
  CHECK(wire::verifyLongSpatial(crypto(), Bytes(got.data(), got.size()), fleetKey()).ok());
  CHECK_EQ(v->payload.size(), 4u + wire::kControlNonceSize);
  return le::readU32(v->payload.data());
}

void runFleetOwnStateCapability() {
  CHECK_EQ(wire::ClientCapability::kFleetOwnState, 8u);
  {
    FakeServer server;
    server.start();
    ManualClock clock;
    FleetConfig cfg = configFor(server);
    cfg.transport.advertiseCapabilities = true;
    FleetSession fleet(cfg, crypto(), clock);
    CHECK(!fleet.fleetOwnState());
    openConnected(fleet);
    // Off by default: signed bundles and bound control only.
    CHECK_EQ(expectCapabilities(server), 0x03u);

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

    // The periodic advertisement carries it, and each carried player's speaker lists follow.
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
    cfg.transport.advertiseCapabilities = true;
    cfg.fleetOwnState = true;
    FleetSession fleet(cfg, crypto(), clock);
    CHECK(fleet.fleetOwnState());
    openConnected(fleet);
    CHECK_EQ(expectCapabilities(server), 0x0Bu);
  }
}

// ---------------------------------------------------------------------------
// A move to another realtime server: the session asks the game server for it, and carries
// its players again there. setServer moves it without asking.
// ---------------------------------------------------------------------------

void runMoveCarriesAgain() {
  FakeServer first, second;
  first.start();
  second.start();
  ManualClock clock;
  FleetConfig cfg = configFor(first);
  int asked = 0;
  cfg.reassign = [&]() -> Result<Assignment> {
    ++asked;
    return Assignment{"127.0.0.1", "", second.port};
  };
  FleetSession fleet(cfg, crypto(), clock);
  Seen seen;
  fleet.setHandlers(recording(seen));
  openConnected(fleet);
  CHECK_EQ(asked, 0);  // open() uses the server it was given
  CHECK_EQ(fleet.server().clientPort, first.port);
  CHECK_EQ(fleet.sessionEpoch(), 1u);

  const std::int64_t a = 401, b = 402;
  carryAs(fleet, first, a, 4001, tokenFor(a));
  carryAs(fleet, first, b, 4002, tokenFor(b));
  CHECK_EQ(seen.results.size(), 2u);

  // The realtime server asks the session to move (COMMAND_RECONNECT, signed with the fleet
  // token).
  std::vector<std::uint8_t> reconnect(wire::kCommandReconnectSize);
  reconnect[0] = 22;
  CHECK(crypto().hmacSha256(asBytes(kFleetToken), Bytes(reconnect.data(), 1), reconnect.data() + 1));
  first.send(reconnect);
  pumpUntil(fleet, [&] { return asked == 1; });
  CHECK_EQ(asked, 1);
  CHECK_EQ(fleet.server().clientPort, second.port);
  CHECK_EQ(fleet.stats().reconnects, 1u);

  // Connected on the second server: both players are carried again there, with fresh proofs.
  clock.advance(100);
  pumpUntil(fleet, [&] { return fleet.state() == ConnState::Connected; });
  CHECK_EQ(fleet.sessionEpoch(), 2u);
  std::vector<std::int64_t> again;
  for (int i = 0; i < 2; ++i) {
    auto got = second.recvOne();
    const std::int64_t id = le::readI64(got.data() + wire::offsets::kPayload);
    const Carry c = readCarry(got, tokenFor(id));
    CHECK_EQ(c.epochMs, kEpochMs + 100);
    again.push_back(c.player);
  }
  CHECK((again == std::vector<std::int64_t>({a, b})) || (again == std::vector<std::int64_t>({b, a})));
  // They stay carried while that is confirmed, so the game keeps sending.
  CHECK(fleet.carries(a) && fleet.carries(b));
  CHECK(fleet.player(a)->pending && fleet.player(b)->pending);
  SpatialSend p;
  p.uuid = uuid('a');
  CHECK(fleet.sendActorUpdate(a, p).ok());
  CHECK_EQ(second.recvOne()[0], 128u);

  // The new server confirms one and refuses the other.
  second.send(carryResultFrame(a, 4001, 0));
  second.send(carryResultFrame(b, 0, 1));
  pumpUntil(fleet, [&] { return seen.results.size() == 4; });
  CHECK_EQ(seen.results.size(), 4u);
  CHECK(fleet.carries(a));
  CHECK(!fleet.player(a)->pending);
  CHECK(!fleet.player(b).has_value());
  CHECK_EQ(fleet.carriedCount(), 1u);

  // setServer moves the session without asking, and carries again.
  CHECK(fleet.setServer(Assignment{"127.0.0.1", "", first.port}).ok());
  pumpUntil(fleet, [&] { return fleet.sessionEpoch() == 3 && fleet.state() == ConnState::Connected; });
  CHECK_EQ(asked, 1);
  CHECK_EQ(fleet.server().clientPort, first.port);
  CHECK_EQ(readCarry(first.recvOne(), tokenFor(a)).player, a);
  first.send(carryResultFrame(a, 4001, 0));
  pumpUntil(fleet, [&] { return !fleet.player(a)->pending; });
  CHECK(fleet.carries(a));

  // Naming the server it is on does nothing; a server without an address is refused.
  CHECK(fleet.setServer(Assignment{"127.0.0.1", "", first.port}).ok());
  settle(fleet);
  CHECK_EQ(fleet.sessionEpoch(), 3u);
  CHECK_EQ(static_cast<int>(fleet.setServer(Assignment{"", "", first.port}).code),
           static_cast<int>(Errc::InvalidArgument));
  CHECK_EQ(static_cast<int>(fleet.setServer(Assignment{"127.0.0.1", "", 0}).code),
           static_cast<int>(Errc::InvalidArgument));

  // A carry again that the new session never answers ends the carry here.
  CHECK(fleet.setServer(Assignment{"127.0.0.1", "", second.port}).ok());
  pumpUntil(fleet, [&] { return fleet.sessionEpoch() == 4 && fleet.state() == ConnState::Connected; });
  readCarry(second.recvOne(), tokenFor(a));
  const std::size_t before = seen.results.size();
  clock.advance(5000);
  fleet.pump(0);
  fleet.poll();
  CHECK_EQ(seen.results.size(), before + 1);
  CHECK_EQ(static_cast<int>(seen.results.back().status), static_cast<int>(FleetCarryStatus::TimedOut));
  CHECK(!fleet.carries(a));
}

// ---------------------------------------------------------------------------
// setToken: a refreshed fleet token (the same token and id, a new expiry) changes nothing
// on the wire; another id carries the players again under it.
// ---------------------------------------------------------------------------

void runSetToken() {
  FakeServer server;
  server.start();
  ManualClock clock;
  FleetSession fleet(configFor(server), crypto(), clock);
  openConnected(fleet);
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

  // Another fleet token and id (a session opened again): the proof names the new id, and
  // the new token signs.
  const std::string nextToken(64, 'n');
  CHECK(fleet.setToken(TokenInfo{nextToken, 9002, 0}).ok());
  auto got = server.recvOne();
  CHECK_EQ(readCarry(got, kPlayerToken, 9002, nextToken).player, kPlayerId);
  CHECK(fleet.carries(kPlayerId));
  CHECK(fleet.player(kPlayerId)->pending);
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
    if (r.size() == 1 && r[0] == kPlayerId) ++texts;
  };
  fleet.setHandlers(std::move(h));
  CHECK(fleet.open().ok());

  // Carried from another thread while the session is still connecting.
  std::thread caller([&] { CHECK(fleet.carry({kPlayerToken, kPlayerId}).ok()); });
  caller.join();
  // The first answer is lost: the network thread sends the carry again.
  CHECK_EQ(readCarry(server.recvOne(), kPlayerToken).player, kPlayerId);
  CHECK_EQ(readCarry(server.recvOne(), kPlayerToken).player, kPlayerId);
  server.send(carryResultFrame(kPlayerId, 77, 0));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (carried.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    fleet.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(carried.load(), 1);
  CHECK(fleet.carries(kPlayerId));

  // Sends from two threads while the game thread polls a delivery.
  std::atomic<int> sent{0};
  auto sender = [&] {
    const std::uint8_t pose[] = {1, 2, 3, 4};
    SpatialSend p;
    p.uuid = uuid('a');
    p.payload = Bytes(pose, sizeof(pose));
    for (int i = 0; i < 50; ++i) {
      if (fleet.sendGenericSpatial(kPlayerId, p).ok()) ++sent;
    }
  };
  std::thread one(sender), two(sender);
  const std::uint8_t body[] = {'h', 'i'};
  server.send(deliveryFrame({kPlayerId}, notification(wire::MessageType::ClientTextNotification, Bytes(body, 2), 1,
                                                      fleetKey())));
  one.join();
  two.join();
  while (texts.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    fleet.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(texts.load(), 1);
  CHECK_EQ(sent.load(), 100);
  CHECK_EQ(fleet.stats().messagesSentByType[140], 100u);
  fleet.close();
}

}  // namespace

int main() {
  runCarrySendRelease();
  runEverySendForAPlayer();
  runCarryResendAndDeadline();
  runCarryStatuses();
  runReleasedReasons();
  runRefreshedTokenReplacesEntry();
  runDeliveryAndPlainState();
  runRequireSignedControl();
  runFleetOwnStateCapability();
  runMoveCarriesAgain();
  runSetToken();
  runOwnedThread();
  std::puts("fleet_session_test OK");
  return 0;
}
