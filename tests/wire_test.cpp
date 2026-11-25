#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/core/crypto.hpp"
#include "crowdy/wire/codec.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::wire;

namespace {

const core::ICrypto& crypto() { return core::opensslCrypto(); }

Token64 testToken() {
  return *Token64::fromString(
      "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+/");
}

core::ActorUuid testUuid() {
  core::ActorUuid u;
  std::memcpy(u.data(), "0123456789abcdef0123456789abcdef", 32);
  return u;
}

// Golden vector: a signed GENERIC_SPATIAL_1 (140) datagram computed
// independently (Python hmac/hashlib) per the public wire-format and HMAC
// docs. appId=7, chunk=(1,-2,3), distance=8, decay=EXPONENTIAL,
// payload=de:ad:be:ef, gameTokenId=123456789, seq=42.
const std::uint8_t kGoldenSpatial[] = {
    0x8c, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x08, 0x01, 0x01, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0xde, 0xad, 0xbe, 0xef, 0x43, 0xec, 0xd4,
    0x68, 0xd2, 0x59, 0x3f, 0x17, 0xf3, 0xfb, 0x06, 0x36, 0x8a, 0x2b, 0x6f, 0x47, 0x32, 0xfc,
    0x2d, 0xa1, 0x65, 0xac, 0xf4, 0xca, 0x2b, 0xfe, 0x16, 0x39, 0x6c, 0x43, 0x36, 0x2a, 0x15,
    0xcd, 0x5b, 0x07, 0x00, 0x00, 0x00, 0x00, 0x2a};

// Golden COMMAND_RECONNECT frame for the same token: HMAC-SHA256(token, the type byte), as the
// server and the game API's UDP proxy compute it. (Until 0.81.0 this vector appended the token
// to the message, a frame no server sends.)
const std::uint8_t kGoldenReconnect[] = {
    0x16, 0xc8, 0x0d, 0x2c, 0xee, 0x89, 0x77, 0x57, 0x81, 0x29, 0xf7,
    0x29, 0x50, 0x01, 0xcd, 0x1b, 0xea, 0x65, 0xdd, 0x11, 0xbe, 0x3f,
    0x51, 0x85, 0x60, 0x52, 0x9c, 0xb2, 0x98, 0xce, 0x5f, 0xc4, 0x2b};

// Golden bound control frames (Buddy v0.45.0) for the same token and nonce 01..08.
const std::uint8_t kGoldenReconnectBound[] = {
    0x2c, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0xda, 0x5c, 0x2e, 0x0f,
    0x16, 0xec, 0x59, 0x2b, 0xd4, 0x1e, 0x81, 0x28, 0x65, 0x27, 0x37, 0x3b, 0x1e, 0xca,
    0xbb, 0x59, 0x1b, 0xcc, 0x7f, 0x64, 0x81, 0xdf, 0xdb, 0x6e, 0xd2, 0xa5, 0xc2};
const std::uint8_t kGoldenReleasedBound[] = {  // reason 5 (idle)
    0x2d, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xfa, 0xe4, 0x50, 0xe0,
    0x83, 0x64, 0x0d, 0xfd, 0xf2, 0x3f, 0xd6, 0xcd, 0x96, 0x3a, 0xef, 0x35, 0xd6, 0xf5,
    0x0b, 0xe3, 0x23, 0x04, 0x62, 0x44, 0x68, 0x25, 0x8d, 0xc7, 0xd8, 0xc1, 0x8e, 0xdf};

void testLayoutConstants() {
  CHECK_EQ(kLongSpatialHeaderSize, 68u);
  CHECK_EQ(kTailNoHmac, 9u);
  CHECK_EQ(kTailWithHmac, 41u);
  CHECK_EQ(kMinLongSpatialNoHmac, 77u);
  CHECK_EQ(kMinLongSpatialWithHmac, 109u);
  CHECK_EQ(channel::kMinRequestSize, 85u);
  CHECK_EQ(channel::kMinNotificationSize, 52u);
  CHECK_EQ(offsets::kUuid, 36u);
  CHECK_EQ(offsets::kPayload, 68u);
  CHECK(isLongSpatialLayout(128) && isLongSpatialLayout(140) && isLongSpatialLayout(142));
  CHECK(isLongSpatialLayout(26));   // heartbeat reuses the layout
  CHECK(isLongSpatialLayout(29));   // CLIENT_CAPABILITIES reuses the layout
  CHECK_EQ(static_cast<int>(MessageType::ClientCapabilities), 29);
  CHECK(!isServerOnlySpatialType(29));  // client-originated; Buddy accepts it
  CHECK(!isLongSpatialLayout(141)); // short spatial reserved
  // v0.30.0: the video pair and actor-left share the layout (Buddy v0.25.0).
  CHECK(isLongSpatialLayout(143) && isLongSpatialLayout(144) && isLongSpatialLayout(145));
  // 0.81.0: the chunk-changed notification (Buddy v0.45.0) shares it too; 147 does not exist.
  CHECK(isLongSpatialLayout(146));
  CHECK(!isLongSpatialLayout(147));
  CHECK_EQ(static_cast<int>(MessageType::ChunkChangedNotification), 146);
  CHECK_EQ(static_cast<int>(MessageType::CommandReconnectBound), 44);
  CHECK_EQ(static_cast<int>(MessageType::CommandSessionReleasedBound), 45);
  CHECK_EQ(static_cast<int>(MessageType::ClientVideoPacket), 143);
  CHECK_EQ(static_cast<int>(MessageType::ClientVideoNotification), 144);
  CHECK_EQ(static_cast<int>(MessageType::ActorLeftNotification), 145);
  CHECK(isServerOnlySpatialType(144) && isServerOnlySpatialType(145) && isServerOnlySpatialType(130));
  CHECK(!isServerOnlySpatialType(143) && !isServerOnlySpatialType(128) && !isServerOnlySpatialType(138));
  const std::uint8_t stale[] = {0}, reserved[] = {1}, unknown[] = {7};
  CHECK_EQ(actorLeftReason(stale, 1), 0u);
  CHECK_EQ(actorLeftReason(reserved, 1), 1u);
  CHECK_EQ(actorLeftReason(unknown, 1), 0u);
  CHECK_EQ(actorLeftReason(nullptr, 0), 0u);
  CHECK(isSpatialType(128) && !isSpatialType(26));
}

void testGoldenEncode() {
  LongSpatialParams p;
  p.type = MessageType::GenericSpatial1;
  p.appId = 7;
  p.chunk = {1, -2, 3};
  p.distance = 8;
  p.decay = DecayRate::Exponential;
  p.uuid = testUuid();
  const std::uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = 123456789;
  p.sequence = 42;

  std::uint8_t buf[256];
  auto n = encodeLongSpatial(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  CHECK_EQ(n.value(), sizeof(kGoldenSpatial));
  CHECK(std::memcmp(buf, kGoldenSpatial, sizeof(kGoldenSpatial)) == 0);
}

// 0.42.0 built CLIENT_CAPABILITIES and then encodeLongSpatial refused opcode 29,
// so the datagram never left. A 4-byte LE flags word of kBundleSigned must encode.
void testEncodeClientCapabilities() {
  LongSpatialParams p;
  p.type = MessageType::ClientCapabilities;
  p.appId = 7;
  p.chunk = {};
  p.distance = 0;
  p.decay = DecayRate::None;
  const std::uint8_t flags[] = {0x01, 0x00, 0x00, 0x00};
  CHECK_EQ(ClientCapability::kBundleSigned, 1u);
  p.payload = Bytes(flags, sizeof(flags));
  p.gameTokenId = 123456789;
  p.sequence = 1;

  std::uint8_t buf[256];
  auto n = encodeLongSpatial(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  CHECK_EQ(buf[offsets::kType], static_cast<std::uint8_t>(MessageType::ClientCapabilities));
  CHECK_EQ(buf[offsets::kPayload + 0], 0x01);
  CHECK_EQ(buf[offsets::kPayload + 1], 0x00);
  CHECK_EQ(buf[offsets::kPayload + 2], 0x00);
  CHECK_EQ(buf[offsets::kPayload + 3], 0x00);

  auto v = parseLongSpatial(Bytes(buf, n.value()));
  CHECK(v.ok());
  CHECK_EQ(static_cast<int>(v->type), static_cast<int>(MessageType::ClientCapabilities));
  CHECK_EQ(v->payload.size(), 4u);
  CHECK_EQ(v->payload[0], 0x01);
  CHECK_EQ(v->payload[1], 0x00);
  CHECK_EQ(v->payload[2], 0x00);
  CHECK_EQ(v->payload[3], 0x00);
}

void testGoldenParseAndVerify() {
  Bytes dg(kGoldenSpatial, sizeof(kGoldenSpatial));
  auto v = parseLongSpatial(dg);
  CHECK(v.ok());
  CHECK_EQ(static_cast<int>(v->type), 140);
  CHECK_EQ(v->appId, 7);
  CHECK_EQ(v->chunk.x, 1);
  CHECK_EQ(v->chunk.y, -2);
  CHECK_EQ(v->chunk.z, 3);
  CHECK_EQ(v->distance, 8u);
  CHECK(v->containsAuth);
  CHECK_EQ(v->payload.size(), 4u);
  CHECK_EQ(v->payload[0], 0xde);
  CHECK_EQ(v->epochMillisOrTokenId, 123456789);
  CHECK_EQ(v->sequence, 42u);
  CHECK(std::memcmp(v->uuid, "0123456789abcdef0123456789abcdef", 32) == 0);

  CHECK(verifyLongSpatial(crypto(), dg, testToken()).ok());

  // Flip one payload byte -> HMAC mismatch.
  std::vector<std::uint8_t> tampered(kGoldenSpatial, kGoldenSpatial + sizeof(kGoldenSpatial));
  tampered[offsets::kPayload] ^= 1;
  CHECK_EQ(static_cast<int>(verifyLongSpatial(crypto(), Bytes(tampered.data(), tampered.size()),
                                              testToken())
                                .code),
           static_cast<int>(Errc::HmacMismatch));

  // Wrong token -> mismatch.
  auto other = *Token64::fromString(std::string(64, 'x'));
  CHECK(!verifyLongSpatial(crypto(), dg, other).ok());
}

void testCommandReconnect() {
  Bytes frame(kGoldenReconnect, sizeof(kGoldenReconnect));
  CHECK(verifyCommandReconnect(crypto(), frame, testToken()).ok());
  auto other = *Token64::fromString(std::string(64, 'x'));
  CHECK(!verifyCommandReconnect(crypto(), frame, other).ok());
  std::vector<std::uint8_t> bad(kGoldenReconnect, kGoldenReconnect + sizeof(kGoldenReconnect));
  bad[5] ^= 0xff;
  CHECK(!verifyCommandReconnect(crypto(), Bytes(bad.data(), bad.size()), testToken()).ok());
}

void testBoundControlFrames() {
  const std::uint8_t nonce[kControlNonceSize] = {1, 2, 3, 4, 5, 6, 7, 8};
  const std::uint8_t other[kControlNonceSize] = {1, 2, 3, 4, 5, 6, 7, 9};
  Bytes reconnect(kGoldenReconnectBound, sizeof(kGoldenReconnectBound));
  CHECK_EQ(sizeof(kGoldenReconnectBound), kCommandReconnectBoundSize);
  CHECK(verifyCommandReconnectBound(crypto(), reconnect, testToken(), nonce).ok());
  CHECK_EQ(static_cast<int>(verifyCommandReconnectBound(crypto(), reconnect, testToken(), other).code),
           static_cast<int>(Errc::Rejected));
  auto otherToken = *Token64::fromString(std::string(64, 'x'));
  CHECK_EQ(static_cast<int>(verifyCommandReconnectBound(crypto(), reconnect, otherToken, nonce).code),
           static_cast<int>(Errc::HmacMismatch));
  CHECK_EQ(static_cast<int>(verifyCommandReconnectBound(crypto(), Bytes(kGoldenReconnect, sizeof(kGoldenReconnect)),
                                                          testToken(), nonce)
                                .code),
           static_cast<int>(Errc::Malformed));

  Bytes released(kGoldenReleasedBound, sizeof(kGoldenReleasedBound));
  CHECK_EQ(sizeof(kGoldenReleasedBound), kCommandSessionReleasedBoundSize);
  auto reason = verifySessionReleasedBound(crypto(), released, testToken(), nonce);
  CHECK(reason.ok());
  CHECK_EQ(reason.value(), 5);
  CHECK(!verifySessionReleasedBound(crypto(), released, testToken(), other).ok());
  std::vector<std::uint8_t> rewritten(kGoldenReleasedBound, kGoldenReleasedBound + sizeof(kGoldenReleasedBound));
  rewritten[1] = 1;  // the reason is inside the signature
  CHECK(!verifySessionReleasedBound(crypto(), Bytes(rewritten.data(), rewritten.size()), testToken(), nonce).ok());

  CHECK_EQ(ClientCapability::kBoundControl, 2u);
  CHECK_EQ(ClientCapability::kAll, 3u);
}

void testChunkChangedPayload() {
  std::uint8_t payload[8];
  le::writeI64(payload, 1'794'400'000'321);
  CHECK_EQ(chunkChangedUpdatedAtMs(Bytes(payload, sizeof(payload))), 1'794'400'000'321);
  CHECK_EQ(chunkChangedUpdatedAtMs(Bytes(payload, 7)), 0);
  CHECK(isLongSpatialLayout(146));
  CHECK(isServerOnlySpatialType(146));
}

void testVoxelPayloadRoundTrip() {
  const std::uint8_t state[] = {1, 2, 3};
  std::uint8_t buf[64];
  auto n = encodeVoxelPayload(-5, 7, 15, 42, Bytes(state, sizeof(state)),
                              MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  CHECK_EQ(n.value(), voxel::kFixedSize + 3);
  auto v = parseVoxelPayload(Bytes(buf, n.value()));
  CHECK(v.ok());
  CHECK_EQ(v->x, -5);
  CHECK_EQ(v->y, 7);
  CHECK_EQ(v->z, 15);
  CHECK_EQ(v->voxelType, 42);
  CHECK_EQ(v->state.size(), 3u);
  CHECK_EQ(v->state[2], 3u);

  // stateLen beyond buffer -> malformed.
  buf[voxel::kStateLenOffset] = 0xff;
  CHECK(!parseVoxelPayload(Bytes(buf, n.value())).ok());
}

void testEventPayloadRoundTrip() {
  const std::uint8_t state[] = {9, 8};
  std::uint8_t buf[16];
  auto n = encodeEventPayload(300, Bytes(state, sizeof(state)), MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  auto v = parseEventPayload(Bytes(buf, n.value()));
  CHECK(v.ok());
  CHECK_EQ(v->eventType, 300u);
  CHECK_EQ(v->state.size(), 2u);
}

void testChannelRoundTrip() {
  ChannelMessageParams p;
  p.channelId = 55;
  p.uuid = testUuid();
  const std::uint8_t payload[] = {'h', 'i'};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = 99;
  p.sequence = 7;

  std::uint8_t buf[256];
  auto n = encodeChannelMessage(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  CHECK_EQ(n.value(), channelRequestSize(2));
  CHECK_EQ(buf[0], 17u);

  // Verify the request HMAC manually: prefix = through containsAuth.
  const std::size_t authOffset = channel::kPayloadOffset + 2;
  CHECK_EQ(buf[authOffset], 1u);
  std::uint8_t expected[kHmacTagSize];
  CHECK(spatialHmac(crypto(), Bytes(buf, authOffset + 1), testToken(), expected));
  CHECK(crypto().constantTimeEquals(expected, buf + authOffset + 1, kHmacTagSize));

  // Build a notification frame and parse it.
  std::uint8_t note[128];
  note[0] = 18;
  le::writeI64(note + channel::kChannelIdOffset, 55);
  std::memcpy(note + channel::kUuidOffset, p.uuid.data(), 32);
  le::writeU16(note + channel::kPayloadLenOffset, 2);
  note[channel::kPayloadOffset] = 'h';
  note[channel::kPayloadOffset + 1] = 'i';
  le::writeI64(note + channel::kPayloadOffset + 2, 1750000000000LL);
  note[channel::kPayloadOffset + 10] = 7;
  auto v = parseChannelNotification(Bytes(note, channel::kPayloadOffset + 11));
  CHECK(v.ok());
  CHECK_EQ(v->channelId, 55);
  CHECK_EQ(v->payload.size(), 2u);
  CHECK_EQ(v->epochMillis, 1750000000000LL);
  CHECK_EQ(v->sequence, 7u);

  // Oversized channel payload rejected.
  std::vector<std::uint8_t> big(channel::kMaxPayload + 1, 0);
  p.payload = Bytes(big.data(), big.size());
  std::vector<std::uint8_t> bigBuf(4096);
  CHECK(!encodeChannelMessage(crypto(), p, testToken(),
                              MutableBytes(bigBuf.data(), bigBuf.size()))
             .ok());
}

std::string toHex(const std::uint8_t* data, std::size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xf]);
  }
  return out;
}

core::ActorUuid uuidOf(const char* s) {
  core::ActorUuid u;
  std::memcpy(u.data(), s, 32);
  return u;
}

void testRangedChannelGolden() {
  CHECK_EQ(channel_ranged::kHeaderSize, 79u);
  CHECK_EQ(channel_ranged::kMinRequestSize, 121u);
  CHECK_EQ(static_cast<int>(MessageType::ChannelMessageRangedRequest), 32);

  // The cross-implementation vector (Buddy's integration builder, cks-game-api's spec and
  // CrowdyJS's binary-wire test pin the same bytes).
  {
    RangedChannelMessageParams p;
    p.channelId = 100;
    p.uuid = uuidOf("uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu");
    p.appId = 2;
    p.origin = {-3, 4, 5};
    p.maxDistance = 12;
    const std::uint8_t payload[] = {'h', 'i'};
    p.payload = Bytes(payload, sizeof(payload));
    p.gameTokenId = 555;
    p.sequence = 9;
    std::uint8_t buf[256];
    auto n = encodeRangedChannelMessage(crypto(), p, *Token64::fromString(std::string(64, 'A')),
                                        MutableBytes(buf, sizeof(buf)));
    CHECK(n.ok());
    CHECK_EQ(n.value(), rangedChannelRequestSize(2));
    CHECK_EQ(toHex(buf, n.value()),
             std::string("20640000000000000075757575757575757575757575757575757575757575757575757575757575"
                         "750200000000000000fdffffffffffffff040000000000000005000000000000000c000000020068"
                         "69017f1c386fd1ec421f0a72745fae881f8adbcb17bcd8a0bb3e446565651d18533d2b0200000000"
                         "000009"));
  }

  // CrowdyJS's binary-wire fixture row, which cks-game-api's generator produced.
  {
    RangedChannelMessageParams p;
    p.channelId = 987654321;
    p.uuid = uuidOf("actor-0123456789abcdef0123456789");
    p.appId = 42;
    p.origin = {3, -2, 7};
    p.maxDistance = 12;
    const std::string payload = "ranged-payload";
    p.payload = Bytes(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    p.gameTokenId = 123456789;
    p.sequence = 34;
    std::uint8_t buf[256];
    auto n = encodeRangedChannelMessage(
        crypto(), p,
        *Token64::fromString("AbCdEfGhIjKlMnOpQrStUvWxYz012345aBcDeFgHiJkLmNoPqRsTuVwXyZ543210"),
        MutableBytes(buf, sizeof(buf)));
    CHECK(n.ok());
    CHECK_EQ(toHex(buf, n.value()),
             std::string("20b168de3a000000006163746f722d30313233343536373839616263646566303132333435363738"
                         "392a000000000000000300000000000000feffffffffffffff07000000000000000c0000000e0072"
                         "616e6765642d7061796c6f616401d267f8843637856bc224a652a7eab03c8fbf5c9622ce941cc405"
                         "1493e408623915cd5b070000000022"));
  }

  // Refused before anything is sent: a distance over the ceiling, a payload over the cap.
  RangedChannelMessageParams p;
  p.uuid = testUuid();
  p.maxDistance = channel_ranged::kMaxDistance;
  std::uint8_t buf[2048];
  CHECK(encodeRangedChannelMessage(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf))).ok());
  p.maxDistance = channel_ranged::kMaxDistance + 1u;
  CHECK(!encodeRangedChannelMessage(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf))).ok());
  p.maxDistance = 5;
  std::vector<std::uint8_t> big(channel::kMaxPayload + 1, 0);
  p.payload = Bytes(big.data(), big.size());
  CHECK(!encodeRangedChannelMessage(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf))).ok());
}

void testGenericError() {
  const std::uint8_t frame[] = {3, 42, 32};
  auto v = parseGenericError(Bytes(frame, sizeof(frame)));
  CHECK(v.ok());
  CHECK_EQ(v->sequence, 42u);
  CHECK_EQ(static_cast<int>(v->code), 32);
  const std::uint8_t shortFrame[] = {3, 42};
  CHECK(!parseGenericError(Bytes(shortFrame, sizeof(shortFrame))).ok());
}

void testBundleIteration() {
  // Bundle with two members: a GenericError and the golden spatial message.
  std::vector<std::uint8_t> bundle;
  bundle.push_back(2);
  const std::uint8_t err[] = {3, 1, 7};
  bundle.push_back(3);
  bundle.push_back(0);
  bundle.insert(bundle.end(), err, err + 3);
  bundle.push_back(static_cast<std::uint8_t>(sizeof(kGoldenSpatial) & 0xff));
  bundle.push_back(static_cast<std::uint8_t>(sizeof(kGoldenSpatial) >> 8));
  bundle.insert(bundle.end(), kGoldenSpatial, kGoldenSpatial + sizeof(kGoldenSpatial));

  int count = 0;
  std::uint8_t types[4] = {};
  auto st = forEachMessage(Bytes(bundle.data(), bundle.size()), [&](Bytes m) {
    types[count++] = m[0];
  });
  CHECK(st.ok());
  CHECK_EQ(count, 2);
  CHECK_EQ(types[0], 3u);
  CHECK_EQ(types[1], 140u);

  // Non-bundle datagram yields itself once.
  count = 0;
  st = forEachMessage(Bytes(kGoldenSpatial, sizeof(kGoldenSpatial)), [&](Bytes) { ++count; });
  CHECK(st.ok());
  CHECK_EQ(count, 1);

  // Truncated bundle: first member delivered, then Malformed.
  bundle.resize(bundle.size() - 10);
  count = 0;
  st = forEachMessage(Bytes(bundle.data(), bundle.size()), [&](Bytes) { ++count; });
  CHECK(!st.ok());
  CHECK_EQ(count, 1);
}

void testBundleWriter() {
  std::uint8_t buf[kMaxDatagramSize];
  BundleWriter w(MutableBytes(buf, sizeof(buf)));
  CHECK(w.empty());
  CHECK_EQ(w.count(), 0u);
  CHECK_EQ(w.size(), kBundleHeaderSize);
  CHECK(w.datagram().empty());
  CHECK_EQ(w.firstType(), 0u);
  CHECK_EQ(kMaxBundleMemberSize, 1229u);

  // A lone member goes out unwrapped: the same bytes the caller appended.
  const std::uint8_t err[] = {3, 1, 7};
  CHECK(w.append(Bytes(err, sizeof(err))));
  CHECK_EQ(w.count(), 1u);
  CHECK_EQ(w.firstType(), 3u);
  Bytes one = w.datagram();
  CHECK_EQ(one.size(), sizeof(err));
  CHECK(std::memcmp(one.data(), err, sizeof(err)) == 0);

  // Two members: the type-2 frame, and forEachMessage gives them back in order.
  CHECK(w.append(Bytes(kGoldenSpatial, sizeof(kGoldenSpatial))));
  CHECK_EQ(w.count(), 2u);
  Bytes two = w.datagram();
  CHECK_EQ(two[0], 2u);
  CHECK_EQ(two.size(), kBundleHeaderSize + (kBundleLengthPrefix + sizeof(err)) +
                           (kBundleLengthPrefix + sizeof(kGoldenSpatial)));
  int count = 0;
  std::size_t sizes[2] = {};
  auto st = forEachMessage(two, [&](Bytes m) {
    if (count < 2) sizes[count] = m.size();
    ++count;
  });
  CHECK(st.ok());
  CHECK_EQ(count, 2);
  CHECK_EQ(sizes[0], sizeof(err));
  CHECK_EQ(sizes[1], sizeof(kGoldenSpatial));
  CHECK(std::memcmp(two.data() + 1 + 2 + sizeof(err) + 2, kGoldenSpatial,
                    sizeof(kGoldenSpatial)) == 0);

  // Zero-length members never fit; reset() empties and re-arms the type byte.
  CHECK(!w.fits(0));
  CHECK(!w.append(Bytes()));
  w.reset();
  CHECK(w.empty());
  CHECK_EQ(buf[0], 2u);

  // Byte capacity: exactly fill 1232, then one more byte is refused untouched.
  const std::vector<std::uint8_t> big(kMaxBundleMemberSize, 0xAB);
  CHECK(w.fits(big.size()));
  CHECK(!w.fits(big.size() + 1));
  CHECK(w.append(Bytes(big.data(), big.size())));
  CHECK_EQ(w.size(), kMaxDatagramSize);
  const std::uint8_t tiny = 9;
  CHECK(!w.fits(1));
  CHECK(!w.append(Bytes(&tiny, 1)));
  CHECK_EQ(w.count(), 1u);
  CHECK_EQ(w.size(), kMaxDatagramSize);
  w.reset();

  // Member cap: 32 tiny members fit, the 33rd is refused.
  for (std::size_t i = 0; i < kMaxBundleMembers; ++i) {
    const std::uint8_t m[] = {128, static_cast<std::uint8_t>(i)};
    CHECK(w.append(Bytes(m, sizeof(m))));
  }
  CHECK_EQ(w.count(), kMaxBundleMembers);
  CHECK(!w.fits(2));
  const std::uint8_t extra[] = {128, 99};
  CHECK(!w.append(Bytes(extra, sizeof(extra))));
  count = 0;
  st = forEachMessage(w.datagram(), [&](Bytes) { ++count; });
  CHECK(st.ok());
  CHECK_EQ(count, 32);
}

void testMalformedInputs() {
  // Empty and tiny datagrams never crash.
  CHECK(!parseLongSpatial(Bytes()).ok());
  const std::uint8_t tiny[] = {140, 1, 2};
  CHECK(!parseLongSpatial(Bytes(tiny, sizeof(tiny))).ok());

  // containsAuth=1 but too short for the HMAC tail.
  std::vector<std::uint8_t> shortAuth(kMinLongSpatialNoHmac, 0);
  shortAuth[0] = 140;
  shortAuth[offsets::kContainsAuth] = 1;
  CHECK(!parseLongSpatial(Bytes(shortAuth.data(), shortAuth.size())).ok());

  // Exhaustive length sweep: no length from 0..300 crashes the parser.
  std::vector<std::uint8_t> sweep(300, 0xab);
  sweep[0] = 140;
  for (std::size_t len = 0; len <= sweep.size(); ++len) {
    (void)parseLongSpatial(Bytes(sweep.data(), len));
    (void)parseChannelNotification(Bytes(sweep.data(), len));
    (void)parseGenericError(Bytes(sweep.data(), len));
    forEachMessage(Bytes(sweep.data(), len), [](Bytes) {});
  }
}

void testRoundTripAllTypes() {
  const MessageType types[] = {
      MessageType::ActorUpdateRequest, MessageType::VoxelUpdateRequest,
      MessageType::ClientAudioPacket,  MessageType::ClientTextPacket,
      MessageType::ClientEventNotification, MessageType::GenericSpatial1,
      MessageType::SingleActorMessage, MessageType::ClientActorHeartbeat,
  };
  for (MessageType t : types) {
    LongSpatialParams p;
    p.type = t;
    p.appId = 1;
    p.chunk = {10, 20, 30};
    p.distance = 4;
    p.uuid = testUuid();
    const std::uint8_t payload[] = {1, 2, 3, 4, 5};
    p.payload = Bytes(payload, sizeof(payload));
    p.gameTokenId = 7;
    p.sequence = 200;

    std::uint8_t buf[256];
    auto n = encodeLongSpatial(crypto(), p, testToken(), MutableBytes(buf, sizeof(buf)));
    CHECK(n.ok());
    auto v = parseLongSpatial(Bytes(buf, n.value()));
    CHECK(v.ok());
    CHECK_EQ(static_cast<int>(v->type), static_cast<int>(t));
    CHECK_EQ(v->payload.size(), 5u);
    CHECK(verifyLongSpatial(crypto(), Bytes(buf, n.value()), testToken()).ok());
  }
}

// A pre-keyed MAC must be an optimisation and nothing else: same key, same
// message, same bytes on the wire. It reuses one context across calls and
// across threads, so it is also the one place where a stale or shared context
// would silently produce a wrong tag rather than fail.
void testPreKeyedMacMatchesOneShot() {
  auto mac = crypto().makeHmacSha256(testToken().bytes());
  CHECK(mac != nullptr);  // the OpenSSL provider offers one

  LongSpatialParams p;
  p.type = MessageType::GenericSpatial1;
  p.appId = 7;
  p.chunk = {1, -2, 3};
  p.distance = 8;
  p.decay = DecayRate::Exponential;
  p.uuid = testUuid();
  const std::uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = 123456789;
  p.sequence = 42;

  // Identical to the independently computed golden vector, not merely to our
  // own one-shot path.
  std::uint8_t keyed[256];
  auto n = encodeLongSpatial(crypto(), p, testToken(), MutableBytes(keyed, sizeof(keyed)),
                             mac.get());
  CHECK(n.ok());
  CHECK_EQ(n.value(), sizeof(kGoldenSpatial));
  CHECK(std::memcmp(keyed, kGoldenSpatial, sizeof(kGoldenSpatial)) == 0);

  // Reuse must be repeatable: a context that is not reset properly typically
  // gets the first one right and the rest wrong.
  for (int round = 0; round < 8; ++round) {
    std::uint8_t again[256];
    auto m = encodeLongSpatial(crypto(), p, testToken(), MutableBytes(again, sizeof(again)),
                               mac.get());
    CHECK(m.ok());
    CHECK(std::memcmp(again, kGoldenSpatial, sizeof(kGoldenSpatial)) == 0);
  }

  // Verification agrees in both directions, and still refuses a tampered frame.
  CHECK(verifyLongSpatial(crypto(), Bytes(keyed, n.value()), testToken(), mac.get()).ok());
  CHECK(verifyLongSpatial(crypto(), Bytes(keyed, n.value()), testToken(), nullptr).ok());
  std::vector<std::uint8_t> tampered(keyed, keyed + n.value());
  tampered[offsets::kPayload] ^= 0xff;
  CHECK_EQ(static_cast<int>(
               verifyLongSpatial(crypto(), Bytes(tampered.data(), tampered.size()), testToken(),
                                 mac.get())
                   .code),
           static_cast<int>(Errc::HmacMismatch));

  // Channel messages sign through the same helper.
  {
    ChannelMessageParams c;
    c.channelId = 55;
    c.uuid = testUuid();
    const std::uint8_t hi[] = {'h', 'i'};
    c.payload = Bytes(hi, sizeof(hi));
    c.gameTokenId = 42;
    c.sequence = 3;
    std::uint8_t withMac[256], withoutMac[256];
    auto a = encodeChannelMessage(crypto(), c, testToken(), MutableBytes(withMac, sizeof(withMac)),
                                  mac.get());
    auto b = encodeChannelMessage(crypto(), c, testToken(),
                                  MutableBytes(withoutMac, sizeof(withoutMac)));
    CHECK(a.ok() && b.ok());
    CHECK_EQ(a.value(), b.value());
    CHECK(std::memcmp(withMac, withoutMac, a.value()) == 0);
  }

  // One MAC, many threads. The implementation keeps a per-thread context, so
  // this is the case that would break if it kept a shared one.
  {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < kPerThread; ++i) {
          std::uint8_t out[256];
          auto r = encodeLongSpatial(crypto(), p, testToken(), MutableBytes(out, sizeof(out)),
                                     mac.get());
          if (!r.ok() || std::memcmp(out, kGoldenSpatial, sizeof(kGoldenSpatial)) != 0)
            mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(mismatches.load(), 0);
  }

  // Two live keys at once must not contaminate each other, which is what a
  // per-thread cache keyed on the wrong thing would do.
  {
    auto other = crypto().makeHmacSha256(
        Token64::fromString(std::string(64, 'z'))->bytes());
    CHECK(other != nullptr);
    for (int round = 0; round < 4; ++round) {
      std::uint8_t a[256], b[256];
      CHECK(encodeLongSpatial(crypto(), p, testToken(), MutableBytes(a, sizeof(a)), mac.get())
                .ok());
      CHECK(encodeLongSpatial(crypto(), p, *Token64::fromString(std::string(64, 'z')),
                              MutableBytes(b, sizeof(b)), other.get())
                .ok());
      CHECK(std::memcmp(a, kGoldenSpatial, sizeof(kGoldenSpatial)) == 0);
      CHECK(std::memcmp(a, b, sizeof(kGoldenSpatial)) != 0);  // different keys, different tags
    }
  }
}


// Buddy v0.30.0: a MESSAGE_BUNDLE_SIGNED verifies at the tail and walks like a bundle.
void testSignedBundle() {
  std::vector<std::uint8_t> dg = {static_cast<std::uint8_t>(MessageType::MessageBundleSigned)};
  const std::uint8_t err[] = {3, 5, 7};
  dg.push_back(3);
  dg.push_back(0);
  dg.insert(dg.end(), err, err + 3);
  dg.push_back(static_cast<std::uint8_t>(sizeof(kGoldenSpatial) & 0xff));
  dg.push_back(static_cast<std::uint8_t>(sizeof(kGoldenSpatial) >> 8));
  dg.insert(dg.end(), kGoldenSpatial, kGoldenSpatial + sizeof(kGoldenSpatial));
  std::uint8_t tag[kHmacTagSize];
  CHECK(spatialHmac(crypto(), Bytes(dg.data(), dg.size()), testToken(), tag));
  dg.insert(dg.end(), tag, tag + kHmacTagSize);

  CHECK(verifySignedBundle(crypto(), Bytes(dg.data(), dg.size()), testToken()).ok());
  auto other = *Token64::fromString(std::string(64, 'x'));
  CHECK_EQ(static_cast<int>(verifySignedBundle(crypto(), Bytes(dg.data(), dg.size()), other).code),
           static_cast<int>(Errc::HmacMismatch));
  std::vector<std::uint8_t> tampered = dg;
  tampered[4] ^= 1;
  CHECK(!verifySignedBundle(crypto(), Bytes(tampered.data(), tampered.size()), testToken()).ok());
  // A plain bundle is not a signed one.
  CHECK(!verifySignedBundle(crypto(), Bytes(kGoldenSpatial, sizeof(kGoldenSpatial)), testToken()).ok());

  // The walk yields the members and never the tail.
  int count = 0;
  std::uint8_t types[4] = {};
  std::size_t lens[4] = {};
  auto st = forEachMessage(Bytes(dg.data(), dg.size()), [&](Bytes m) {
    if (count < 4) {
      types[count] = m[0];
      lens[count] = m.size();
    }
    ++count;
  });
  CHECK(st.ok());
  CHECK_EQ(count, 2);
  CHECK_EQ(types[0], 3u);
  CHECK_EQ(lens[0], 3u);
  CHECK_EQ(types[1], 140u);
  CHECK_EQ(lens[1], sizeof(kGoldenSpatial));
}

}  // namespace

void testChannelAudio() {
  CHECK_EQ(static_cast<int>(MessageType::ChannelAudioRequest), 35);
  CHECK_EQ(static_cast<int>(MessageType::ChannelAudioNotification), 36);
  CHECK_EQ(static_cast<int>(ErrorCode::AppPaused), 33);

  // The vector CrowdyJS's channel-audio test pins (its serializeChannelAudio, opcode 17's
  // builder with type byte 35): channel 4242, uuid "v"x32, payload 01..05, token "T"x64,
  // gameTokenId 777, seq 200.
  ChannelMessageParams p;
  p.channelId = 4242;
  p.uuid = uuidOf("vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv");
  const std::uint8_t payload[] = {1, 2, 3, 4, 5};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = 777;
  p.sequence = 200;
  std::uint8_t audio[256];
  auto n = encodeChannelAudio(crypto(), p, *Token64::fromString(std::string(64, 'T')),
                              MutableBytes(audio, sizeof(audio)));
  CHECK(n.ok());
  CHECK_EQ(toHex(audio, n.value()),
           std::string("23921000000000000076767676767676767676767676767676767676767676767676767676767676"
                       "760500010203040501f7c505bbdfc07169f661cb3e3c8d061bbf0065ef4bdc239df40c77112ff8"
                       "85150903000000000000c8"));

  // Same layout as 17 apart from the type byte (and the HMAC, which covers it).
  std::uint8_t message[256];
  auto m = encodeChannelMessage(crypto(), p, *Token64::fromString(std::string(64, 'T')),
                                MutableBytes(message, sizeof(message)));
  CHECK(m.ok());
  CHECK_EQ(m.value(), n.value());
  CHECK_EQ(message[0], 17u);
  CHECK(std::memcmp(audio + 1, message + 1, channel::kPayloadOffset + sizeof(payload)) == 0);
  CHECK(!encodeChannelRequest(MessageType::ChannelMessageNotification, crypto(), p,
                              testToken(), MutableBytes(audio, sizeof(audio)))
             .ok());
  std::vector<std::uint8_t> big(channel::kMaxPayload + 1, 0);
  p.payload = Bytes(big.data(), big.size());
  std::vector<std::uint8_t> bigBuf(4096);
  CHECK(!encodeChannelAudio(crypto(), p, testToken(), MutableBytes(bigBuf.data(), bigBuf.size()))
             .ok());

  // A 36 parses like an 18; datagram[0] says which.
  std::uint8_t note[64];
  note[0] = 36;
  le::writeI64(note + channel::kChannelIdOffset, 9);
  std::memcpy(note + channel::kUuidOffset, "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv", 32);
  le::writeU16(note + channel::kPayloadLenOffset, 2);
  note[channel::kPayloadOffset] = 0xaa;
  note[channel::kPayloadOffset + 1] = 0xbb;
  le::writeI64(note + channel::kPayloadOffset + 2, 1700000000555LL);
  note[channel::kPayloadOffset + 10] = 4;
  auto v = parseChannelNotification(Bytes(note, channel::kPayloadOffset + 11));
  CHECK(v.ok());
  CHECK_EQ(v->channelId, 9);
  CHECK_EQ(v->payload.size(), 2u);
  CHECK_EQ(v->sequence, 4u);
  note[0] = 35;
  CHECK(!parseChannelNotification(Bytes(note, channel::kPayloadOffset + 11)).ok());

  const std::uint8_t paused[] = {3, 9, 33};
  auto err = parseGenericError(Bytes(paused, sizeof(paused)));
  CHECK(err.ok());
  CHECK(err->code == ErrorCode::AppPaused);
}

// ---------------------------------------------------------------------------
// Fleet sessions (DESIGN-2026-11-fleet-session section 6; the contract's section 3).
//
// The vectors were computed independently (Python hmac/hashlib) from the written formats:
//   fleet token   "fleet-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP", gameTokenId 9001
//   player token  "player-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO", gameTokenId 4242
//   appId 7, epoch millis 1795000000000
// fleet_session_test holds FleetSession to the same bytes.
// ---------------------------------------------------------------------------

const char kFleetToken[] = "fleet-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP";
const char kPlayerToken[] = "player-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO";
constexpr std::int64_t kFleetId = 9001;
constexpr std::int64_t kPlayerId = 4242;
constexpr std::int64_t kCarryEpochMs = 1795000000000LL;

// proof = HMAC-SHA256(player token, "ck-fleet-carry" + 9001 + 4242 + 1795000000000)
const char kGoldenCarryProof[] =
    "5602abb9300581f6a20e247ea0b406f690646a3d27f9427e95c5f6363115b35a";
// FLEET_CARRY (49), sequence 0: the capabilities' frame, the fleet id in the tail.
const char kGoldenFleetCarry[] =
    "3107000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000009210000000000000005e56eea10100005602abb9"
    "300581f6a20e247ea0b406f690646a3d27f9427e95c5f6363115b35a9d69b5b07c19a2450e61725d9995c19c"
    "940e41fa602fcce2535e780e5d2d81e3292300000000000000";
// FLEET_RELEASE (50), sequence 2.
const char kGoldenFleetRelease[] =
    "3207000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000009210000000000000903cf330c14c7344619b97d3"
    "736c97d042c2669c9c8c1ed8e4b1922c327f684d292300000000000002";
// ACTOR_UPDATE_REQUEST (128) for the carried player, sequence 1: chunk (1,-2,3), distance 8,
// uuid "0123456789abcdef0123456789abcdef", payload de ad be ef. The tail's id is the
// PLAYER's (4242) and the HMAC is keyed with the FLEET token.
const char kGoldenCarriedActorUpdate[] =
    "8007000000000000000100000000000000feffffffffffffff03000000000000000800013031323334353637"
    "383961626364656630313233343536373839616263646566deadbeefafca3c1e53f866c6384fb186e2396664"
    "b62d7acd0fee6d9445b96e283d1c09f4921000000000000001";

void testFleetConstants() {
  CHECK_EQ(static_cast<int>(MessageType::FleetCarry), 49);
  CHECK_EQ(static_cast<int>(MessageType::FleetRelease), 50);
  CHECK_EQ(static_cast<int>(MessageType::FleetCarryResult), 51);
  CHECK_EQ(static_cast<int>(MessageType::FleetReleased), 52);
  CHECK_EQ(static_cast<int>(MessageType::FleetDelivery), 53);
  // The two requests travel in the long-spatial layout; the three answers do not.
  CHECK(isLongSpatialLayout(49) && isLongSpatialLayout(50));
  CHECK(!isLongSpatialLayout(48) && !isLongSpatialLayout(51) && !isLongSpatialLayout(52) &&
        !isLongSpatialLayout(53));
  CHECK(isClientOnlyControlType(29) && isClientOnlyControlType(47) && isClientOnlyControlType(49) &&
        isClientOnlyControlType(50));
  CHECK(!isClientOnlyControlType(26) && !isClientOnlyControlType(51) && !isClientOnlyControlType(128));
  CHECK_EQ(ClientCapability::kFleetOwnState, 8u);
  CHECK_EQ(ClientCapability::kAll & ClientCapability::kFleetOwnState, 0u);
  CHECK_EQ(fleet::kCarryProofLabelSize, 14u);
  CHECK_EQ(fleet::kCarryProofMessageSize, 38u);
  CHECK_EQ(fleet::kCarryPayloadSize, 48u);
  CHECK_EQ(fleet::kCarryResultSize, 18u);
  CHECK_EQ(fleet::kReleasedSize, 10u);
  CHECK_EQ(fleet::kMaxDeliveryRecipients, 32u);
}

void testFleetCarryGolden() {
  const Token64 fleetToken = *Token64::fromString(kFleetToken);
  const Token64 playerToken = *Token64::fromString(kPlayerToken);

  // The proof, against an HMAC computed here from the written format.
  std::uint8_t proof[kHmacTagSize];
  CHECK(fleetCarryProof(crypto(), playerToken, kFleetId, kPlayerId, kCarryEpochMs, proof));
  CHECK_EQ(toHex(proof, sizeof(proof)), std::string(kGoldenCarryProof));
  std::vector<std::uint8_t> message = {'c', 'k', '-', 'f', 'l', 'e', 'e', 't', '-', 'c', 'a', 'r', 'r', 'y'};
  for (std::int64_t number : {kFleetId, kPlayerId, kCarryEpochMs}) {
    for (int i = 0; i < 8; ++i)
      message.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(number) >> (8 * i)) & 0xff));
  }
  CHECK_EQ(message.size(), 38u);
  std::uint8_t independent[kHmacTagSize];
  CHECK(crypto().hmacSha256(asBytes(kPlayerToken), Bytes(message.data(), message.size()), independent));
  CHECK(std::memcmp(proof, independent, sizeof(proof)) == 0);
  // Keyed with the player's token: the fleet token gives another proof.
  std::uint8_t wrongKey[kHmacTagSize];
  CHECK(fleetCarryProof(crypto(), fleetToken, kFleetId, kPlayerId, kCarryEpochMs, wrongKey));
  CHECK(std::memcmp(proof, wrongKey, sizeof(proof)) != 0);
  // Each of the three numbers is in it.
  std::uint8_t other[kHmacTagSize];
  CHECK(fleetCarryProof(crypto(), playerToken, kFleetId + 1, kPlayerId, kCarryEpochMs, other));
  CHECK(std::memcmp(proof, other, sizeof(proof)) != 0);
  CHECK(fleetCarryProof(crypto(), playerToken, kFleetId, kPlayerId + 1, kCarryEpochMs, other));
  CHECK(std::memcmp(proof, other, sizeof(proof)) != 0);
  CHECK(fleetCarryProof(crypto(), playerToken, kFleetId, kPlayerId, kCarryEpochMs + 1, other));
  CHECK(std::memcmp(proof, other, sizeof(proof)) != 0);

  // The payload and the whole message.
  std::uint8_t payload[fleet::kCarryPayloadSize];
  auto pn = encodeFleetCarryPayload(crypto(), playerToken, kFleetId, kPlayerId, kCarryEpochMs,
                                    MutableBytes(payload, sizeof(payload)));
  CHECK(pn.ok());
  CHECK_EQ(pn.value(), 48u);
  CHECK_EQ(le::readI64(payload), kPlayerId);
  CHECK_EQ(le::readI64(payload + 8), kCarryEpochMs);
  CHECK(std::memcmp(payload + 16, proof, sizeof(proof)) == 0);
  std::uint8_t small[47];
  CHECK_EQ(static_cast<int>(encodeFleetCarryPayload(crypto(), playerToken, kFleetId, kPlayerId,
                                                    kCarryEpochMs, MutableBytes(small, sizeof(small)))
                                .error()),
           static_cast<int>(Errc::BufferTooSmall));

  LongSpatialParams p;
  p.type = MessageType::FleetCarry;
  p.appId = 7;
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = kFleetId;
  p.sequence = 0;
  std::uint8_t buf[256];
  auto n = encodeLongSpatial(crypto(), p, fleetToken, MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  CHECK_EQ(n.value(), 157u);
  CHECK_EQ(toHex(buf, n.value()), std::string(kGoldenFleetCarry));
  // Signed with the fleet token, as every message of the session is.
  CHECK(verifyLongSpatial(crypto(), Bytes(buf, n.value()), fleetToken).ok());
  CHECK_EQ(static_cast<int>(verifyLongSpatial(crypto(), Bytes(buf, n.value()), playerToken).code),
           static_cast<int>(Errc::HmacMismatch));
  auto v = parseLongSpatial(Bytes(buf, n.value()));
  CHECK(v.ok());
  CHECK_EQ(v->epochMillisOrTokenId, kFleetId);
  CHECK_EQ(v->payload.size(), 48u);

  // FLEET_RELEASE.
  std::uint8_t releasePayload[fleet::kReleasePayloadSize];
  auto rn = encodeFleetReleasePayload(kPlayerId, MutableBytes(releasePayload, sizeof(releasePayload)));
  CHECK(rn.ok());
  CHECK_EQ(rn.value(), 8u);
  p.type = MessageType::FleetRelease;
  p.payload = Bytes(releasePayload, sizeof(releasePayload));
  p.sequence = 2;
  n = encodeLongSpatial(crypto(), p, fleetToken, MutableBytes(buf, sizeof(buf)));
  CHECK(n.ok());
  CHECK_EQ(n.value(), 117u);
  CHECK_EQ(toHex(buf, n.value()), std::string(kGoldenFleetRelease));
}

// A message for a carried player: what the player's own session would send, with the HMAC
// keyed by the fleet token. The tail's id is the player's either way.
void testFleetCarriedActorUpdateGolden() {
  const Token64 fleetToken = *Token64::fromString(kFleetToken);
  const Token64 playerToken = *Token64::fromString(kPlayerToken);
  LongSpatialParams p;
  p.type = MessageType::ActorUpdateRequest;
  p.appId = 7;
  p.chunk = {1, -2, 3};
  p.distance = 8;
  p.uuid = testUuid();
  const std::uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
  p.payload = Bytes(payload, sizeof(payload));
  p.gameTokenId = kPlayerId;
  p.sequence = 1;
  std::uint8_t carried[256];
  auto n = encodeLongSpatial(crypto(), p, fleetToken, MutableBytes(carried, sizeof(carried)));
  CHECK(n.ok());
  CHECK_EQ(n.value(), 113u);
  CHECK_EQ(toHex(carried, n.value()), std::string(kGoldenCarriedActorUpdate));
  CHECK(verifyLongSpatial(crypto(), Bytes(carried, n.value()), fleetToken).ok());
  CHECK_EQ(static_cast<int>(verifyLongSpatial(crypto(), Bytes(carried, n.value()), playerToken).code),
           static_cast<int>(Errc::HmacMismatch));

  // The player's own session's message differs in the 32 HMAC bytes and nowhere else.
  std::uint8_t own[256];
  auto on = encodeLongSpatial(crypto(), p, playerToken, MutableBytes(own, sizeof(own)));
  CHECK(on.ok());
  CHECK_EQ(on.value(), n.value());
  const std::size_t hmacAt = n.value() - kTailWithHmac;
  CHECK(std::memcmp(carried, own, hmacAt) == 0);
  CHECK(std::memcmp(carried + hmacAt, own + hmacAt, kHmacTagSize) != 0);
  CHECK(std::memcmp(carried + hmacAt + kHmacTagSize, own + hmacAt + kHmacTagSize, kTailNoHmac) == 0);
}

void testFleetServerMessages() {
  // FLEET_CARRY_RESULT: [51][player][user][status].
  std::uint8_t result[fleet::kCarryResultSize] = {51};
  le::writeI64(result + 1, kPlayerId);
  le::writeI64(result + 9, 77);
  result[17] = 0;
  auto r = parseFleetCarryResult(Bytes(result, sizeof(result)));
  CHECK(r.ok());
  CHECK_EQ(r->playerGameTokenId, kPlayerId);
  CHECK_EQ(r->userId, 77);
  CHECK_EQ(r->status, 0u);
  for (std::uint8_t status = 1; status <= 4; ++status) {
    result[17] = status;
    r = parseFleetCarryResult(Bytes(result, sizeof(result)));
    CHECK(r.ok());
    CHECK_EQ(r->status, status);
  }
  CHECK(!parseFleetCarryResult(Bytes(result, sizeof(result) - 1)).ok());
  result[0] = 52;
  CHECK(!parseFleetCarryResult(Bytes(result, sizeof(result))).ok());

  // FLEET_RELEASED: [52][player][reason].
  std::uint8_t released[fleet::kReleasedSize] = {52};
  le::writeI64(released + 1, kPlayerId);
  for (std::uint8_t reason : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{5}}) {
    released[9] = reason;
    auto rel = parseFleetReleased(Bytes(released, sizeof(released)));
    CHECK(rel.ok());
    CHECK_EQ(rel->playerGameTokenId, kPlayerId);
    CHECK_EQ(rel->reason, reason);
  }
  CHECK(!parseFleetReleased(Bytes(released, sizeof(released) - 1)).ok());
  released[0] = 51;
  CHECK(!parseFleetReleased(Bytes(released, sizeof(released))).ok());

  // FLEET_DELIVERY: [53][count][count x player][notification].
  auto delivery = [](std::size_t count, std::size_t listed, Bytes notification) {
    std::vector<std::uint8_t> f(2 + listed * 8 + notification.size());
    f[0] = 53;
    f[1] = static_cast<std::uint8_t>(count);
    for (std::size_t i = 0; i < listed; ++i)
      le::writeI64(f.data() + 2 + i * 8, 1000 + static_cast<std::int64_t>(i));
    if (!notification.empty())
      std::memcpy(f.data() + 2 + listed * 8, notification.data(), notification.size());
    return f;
  };
  const Bytes note(kGoldenSpatial, sizeof(kGoldenSpatial));
  auto one = delivery(1, 1, note);
  auto d = parseFleetDelivery(Bytes(one.data(), one.size()));
  CHECK(d.ok());
  CHECK_EQ(d->count, 1u);
  CHECK_EQ(d->recipient(0), 1000);
  CHECK_EQ(d->notification.size(), sizeof(kGoldenSpatial));
  CHECK(std::memcmp(d->notification.data(), kGoldenSpatial, sizeof(kGoldenSpatial)) == 0);

  auto most = delivery(32, 32, note);
  d = parseFleetDelivery(Bytes(most.data(), most.size()));
  CHECK(d.ok());
  CHECK_EQ(d->count, 32u);
  for (std::size_t i = 0; i < 32; ++i) CHECK_EQ(d->recipient(i), 1000 + static_cast<std::int64_t>(i));
  CHECK_EQ(d->notification[0], 140u);
  CHECK_EQ(d->notification.size(), sizeof(kGoldenSpatial));

  // A count of 0, a count past 32, a list shorter than its count, and no notification.
  auto none = delivery(0, 0, note);
  CHECK(!parseFleetDelivery(Bytes(none.data(), none.size())).ok());
  auto tooMany = delivery(33, 33, note);
  CHECK(!parseFleetDelivery(Bytes(tooMany.data(), tooMany.size())).ok());
  auto shortList = delivery(4, 2, Bytes());
  CHECK(!parseFleetDelivery(Bytes(shortList.data(), shortList.size())).ok());
  auto bare = delivery(2, 2, Bytes());
  CHECK(!parseFleetDelivery(Bytes(bare.data(), bare.size())).ok());
  const std::uint8_t header[] = {53};
  CHECK(!parseFleetDelivery(Bytes(header, sizeof(header))).ok());
  one[0] = 51;
  CHECK(!parseFleetDelivery(Bytes(one.data(), one.size())).ok());
}

int main() {
  testFleetConstants();
  testFleetCarryGolden();
  testFleetCarriedActorUpdateGolden();
  testFleetServerMessages();
  testLayoutConstants();
  testGoldenEncode();
  testEncodeClientCapabilities();
  testGoldenParseAndVerify();
  testCommandReconnect();
  testBoundControlFrames();
  testChunkChangedPayload();
  testVoxelPayloadRoundTrip();
  testEventPayloadRoundTrip();
  testChannelRoundTrip();
  testRangedChannelGolden();
  testChannelAudio();
  testGenericError();
  testBundleIteration();
  testBundleWriter();
  testMalformedInputs();
  testRoundTripAllTypes();
  testPreKeyedMacMatchesOneShot();
  testSignedBundle();
  std::puts("wire_test OK");
  return 0;
}
