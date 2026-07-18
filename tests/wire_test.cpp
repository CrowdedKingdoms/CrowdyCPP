#include <cstring>
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

// Golden COMMAND_RECONNECT frame for the same token.
const std::uint8_t kGoldenReconnect[] = {
    0x16, 0x74, 0x8a, 0x0e, 0x41, 0x6b, 0xaa, 0x63, 0xbe, 0xfb, 0xa6,
    0xff, 0x5f, 0x52, 0x61, 0x1c, 0xc0, 0x93, 0x85, 0x5e, 0x04, 0xe6,
    0x0a, 0xc0, 0x57, 0xe1, 0x4e, 0x8f, 0xbe, 0x19, 0x82, 0x81, 0xe2};

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
  CHECK(!isLongSpatialLayout(141)); // short spatial reserved
  CHECK(!isLongSpatialLayout(143));
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

}  // namespace

int main() {
  testLayoutConstants();
  testGoldenEncode();
  testGoldenParseAndVerify();
  testCommandReconnect();
  testVoxelPayloadRoundTrip();
  testEventPayloadRoundTrip();
  testChannelRoundTrip();
  testGenericError();
  testBundleIteration();
  testMalformedInputs();
  testRoundTripAllTypes();
  std::puts("wire_test OK");
  return 0;
}
