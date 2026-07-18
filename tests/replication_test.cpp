// Offline replication-client test: a fake in-process UDP "server" on
// 127.0.0.1 receives the client's signed messages, verifies them, and replies
// with signed notifications, bundles, error frames, and a COMMAND_RECONNECT —
// exercising Connection's full lifecycle in manual-pump mode.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "crowdy/replication/connection.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

const std::string kToken(64, 't');

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

  std::vector<std::uint8_t> recvOne(int timeoutMs = 2000) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    lastClientLen = sizeof(lastClient);
    const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&lastClient), &lastClientLen);
    CHECK(n > 0);
    return std::vector<std::uint8_t>(buf, buf + n);
  }

  void sendToClient(const std::uint8_t* data, std::size_t len) {
    CHECK(lastClientLen > 0);
    CHECK(::sendto(fd, data, len, 0, reinterpret_cast<sockaddr*>(&lastClient), lastClientLen) ==
          static_cast<ssize_t>(len));
  }
};

struct StubProvider final : ISessionProvider {
  int port;
  int assignCalls = 0;
  explicit StubProvider(int p) : port(p) {}
  Result<Assignment> assignServer() override {
    ++assignCalls;
    return Assignment{"127.0.0.1", "", port};
  }
  Result<TokenInfo> refreshToken() override {
    return TokenInfo{kToken, 42, 0};
  }
};

wire::Token64 token64() { return *wire::Token64::fromString(kToken); }

core::ActorUuid uuid(char fill) {
  core::ActorUuid u;
  std::memset(u.data(), fill, 32);
  return u;
}

// Build a signed server->client notification (epoch millis in the tail slot).
std::vector<std::uint8_t> makeNotification(wire::MessageType type, Bytes payload,
                                           std::int64_t epochMs, std::uint8_t seq) {
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = 7;
  p.chunk = {1, 2, 3};
  p.distance = 8;
  p.uuid = uuid('b');
  p.payload = payload;
  p.gameTokenId = epochMs;  // same tail slot
  p.sequence = seq;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(payload.size()));
  auto n = wire::encodeLongSpatial(core::opensslCrypto(), p, token64(),
                                   MutableBytes(buf.data(), buf.size()));
  CHECK(n.ok());
  buf.resize(n.value());
  return buf;
}

void run() {
  FakeServer server;
  server.start();
  auto provider = std::make_shared<StubProvider>(server.port);

  Config cfg;
  cfg.appId = 7;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;

  Connection conn(cfg, provider);

  int actorUpdates = 0, voxelUpdates = 0, errors = 0, statusChanges = 0;
  std::vector<ConnState> states;
  Handlers handlers;
  handlers.actorUpdate = [&](const SpatialNotification& n) {
    ++actorUpdates;
    CHECK_EQ(n.appId, 7);
    CHECK_EQ(n.chunk.x, 1);
    CHECK_EQ(n.payload.size(), 4u);
    CHECK_EQ(n.epochMillis, 1700000000000LL);
  };
  handlers.voxelUpdate = [&](const SpatialNotification&, const wire::VoxelPayloadView& v) {
    ++voxelUpdates;
    CHECK_EQ(v.voxelType, 9);
  };
  handlers.genericError = [&](const GenericError& e) {
    ++errors;
    CHECK_EQ(static_cast<int>(e.code), 7);  // UNAUTHORIZED
    CHECK_EQ(e.sequence, 200u);
  };
  handlers.status = [&](ConnState s) {
    ++statusChanges;
    states.push_back(s);
  };
  conn.setHandlers(std::move(handlers));

  CHECK(conn.connect().ok());
  CHECK_EQ(provider->assignCalls, 1);

  // --- Client -> server: actor update is well-formed and verifiable.
  const std::uint8_t pose[] = {1, 2, 3, 4};
  SpatialSend send;
  send.chunk = {1, 2, 3};
  send.uuid = uuid('a');
  send.payload = Bytes(pose, sizeof(pose));
  auto seq = conn.sendActorUpdate(send);
  CHECK(seq.ok());

  auto received = server.recvOne();
  CHECK_EQ(received[0], 128u);  // ACTOR_UPDATE_REQUEST
  auto parsed = wire::parseLongSpatial(Bytes(received.data(), received.size()));
  CHECK(parsed.ok());
  CHECK_EQ(parsed->appId, 7);
  CHECK_EQ(parsed->epochMillisOrTokenId, 42);  // gameTokenId in C->S tail
  CHECK(wire::verifyLongSpatial(core::opensslCrypto(), Bytes(received.data(), received.size()),
                                token64())
            .ok());

  // Heartbeat reuses the layout with opcode 26.
  CHECK(conn.sendHeartbeat({1, 2, 3}, uuid('a')).ok());
  CHECK_EQ(server.recvOne()[0], 26u);

  // Channel publish.
  const std::uint8_t hi[] = {'h', 'i'};
  CHECK(conn.sendChannelMessage(55, uuid('a'), Bytes(hi, sizeof(hi))).ok());
  CHECK_EQ(server.recvOne()[0], 17u);

  // --- Server -> client: single notification.
  auto note = makeNotification(wire::MessageType::ActorUpdateNotification,
                               Bytes(pose, sizeof(pose)), 1700000000000LL, 5);
  server.sendToClient(note.data(), note.size());

  // Bundle: voxel notification + error frame.
  std::uint8_t voxelPayload[wire::voxel::kFixedSize];
  wire::encodeVoxelPayload(1, 2, 3, 9, Bytes(), MutableBytes(voxelPayload, sizeof(voxelPayload)));
  auto voxelNote = makeNotification(wire::MessageType::VoxelUpdateNotification,
                                    Bytes(voxelPayload, sizeof(voxelPayload)), 1700000000001LL, 6);
  std::vector<std::uint8_t> bundle;
  bundle.push_back(2);
  bundle.push_back(static_cast<std::uint8_t>(voxelNote.size() & 0xff));
  bundle.push_back(static_cast<std::uint8_t>(voxelNote.size() >> 8));
  bundle.insert(bundle.end(), voxelNote.begin(), voxelNote.end());
  const std::uint8_t errFrame[] = {3, 200, 7};
  bundle.push_back(3);
  bundle.push_back(0);
  bundle.insert(bundle.end(), errFrame, errFrame + 3);
  server.sendToClient(bundle.data(), bundle.size());

  // Tampered notification must be dropped (HMAC mismatch).
  auto tampered = note;
  tampered[wire::offsets::kPayload] ^= 0xff;
  server.sendToClient(tampered.data(), tampered.size());

  // Pump + poll until everything arrives.
  for (int i = 0; i < 100 && (actorUpdates + voxelUpdates + errors) < 3; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(actorUpdates, 1);
  CHECK_EQ(voxelUpdates, 1);
  CHECK_EQ(errors, 1);
  CHECK_EQ(conn.stats().hmacFailures, 1u);
  CHECK_EQ(conn.stats().lastServerEpochMs, 1700000000001LL);
  CHECK(statusChanges >= 1);  // Connecting -> Connected observed
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Connected));

  // --- COMMAND_RECONNECT: verified command triggers reassignment.
  std::uint8_t rc[wire::kCommandReconnectSize];
  rc[0] = 22;
  std::uint8_t msg[1 + wire::kTokenOctets];
  msg[0] = 22;
  std::memcpy(msg + 1, kToken.data(), 64);
  CHECK(core::opensslCrypto().hmacSha256(asBytes(kToken), Bytes(msg, sizeof(msg)), rc + 1));
  server.sendToClient(rc, sizeof(rc));

  for (int i = 0; i < 100 && provider->assignCalls < 2; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(provider->assignCalls, 2);
  CHECK_EQ(conn.stats().reconnects, 1u);

  // Sends still work after reassignment.
  CHECK(conn.sendActorUpdate(send).ok());
  CHECK_EQ(server.recvOne()[0], 128u);

  // Forged reconnect (bad HMAC) must NOT trigger reassignment.
  rc[5] ^= 0xff;
  server.sendToClient(rc, sizeof(rc));
  for (int i = 0; i < 5; ++i) {
    conn.pump(20);
    conn.poll();
  }
  CHECK_EQ(provider->assignCalls, 2);

  // --- AndWait: a queued self-echo with the send's sequence resolves the wait.
  conn.setHandlers({});  // the strict handlers above do not apply to this traffic
  auto seqForWait = conn.sendActorUpdate(send);
  CHECK(seqForWait.ok());
  auto sentDatagram = server.recvOne();
  auto parsedSent = wire::parseLongSpatial(Bytes(sentDatagram.data(), sentDatagram.size()));
  CHECK(parsedSent.ok());
  auto echo = makeNotification(wire::MessageType::ActorUpdateNotification,
                               Bytes(pose, sizeof(pose)), 1700000001000LL, parsedSent->sequence);
  std::memcpy(echo.data() + wire::offsets::kUuid, send.uuid.data(), 32);
  // Re-sign after patching the uuid (prefix changed).
  {
    const std::size_t prefixLen = echo.size() - wire::kTailWithHmac;
    CHECK(wire::spatialHmac(core::opensslCrypto(), Bytes(echo.data(), prefixLen), token64(),
                            echo.data() + prefixLen));
  }
  server.sendToClient(echo.data(), echo.size());
  auto outcome = conn.waitForSequence(parsedSent->sequence, send.uuid, 2000);
  CHECK(outcome.acknowledged);
  CHECK(!outcome.error.has_value());
  CHECK_EQ(outcome.serverEpochMs, 1700000001000LL);

  // AndWait: a correlated error resolves with the code instead.
  auto seqForError = conn.sendActorUpdate(send);
  CHECK(seqForError.ok());
  server.recvOne();
  const std::uint8_t errReply[] = {3, seqForError.value(), 7};
  server.sendToClient(errReply, sizeof(errReply));
  auto errOutcome = conn.waitForSequence(seqForError.value(), send.uuid, 2000);
  CHECK(!errOutcome.acknowledged);
  CHECK(errOutcome.error.has_value());
  CHECK_EQ(static_cast<int>(*errOutcome.error), 7);

  conn.disconnect();
  CHECK_EQ(static_cast<int>(conn.state()), static_cast<int>(ConnState::Closed));
}

}  // namespace

int main() {
  run();
  std::puts("replication_test OK");
  return 0;
}
