// Offline session-layer test over the fake-server pattern from
// replication_test: two "players" through one fake server would need real
// fan-out, so instead the fake server replays notifications and we assert
// the stores (self send loop, remote registry, chunk merge, inboxes, error
// attribution) behave per the World Stores contract.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "crowdy/session/world_session.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::session;
using namespace crowdy::replication;

namespace {

const std::string kToken(64, 't');
wire::Token64 token64() { return *wire::Token64::fromString(kToken); }

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
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
  }
  ~FakeServer() {
    if (fd >= 0) ::close(fd);
  }

  std::vector<std::uint8_t> recvOne(int timeoutMs = 2000) {
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::uint8_t buf[2048];
    clientLen = sizeof(client);
    const ssize_t n =
        ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&client), &clientLen);
    CHECK(n > 0);
    return std::vector<std::uint8_t>(buf, buf + n);
  }
  void reply(const std::uint8_t* data, std::size_t len) {
    CHECK(::sendto(fd, data, len, 0, reinterpret_cast<sockaddr*>(&client), clientLen) ==
          static_cast<ssize_t>(len));
  }
};

struct StubProvider final : ISessionProvider {
  int port;
  explicit StubProvider(int p) : port(p) {}
  Result<Assignment> assignServer() override { return Assignment{"127.0.0.1", "", port}; }
  Result<TokenInfo> refreshToken() override { return TokenInfo{kToken, 42, 0}; }
};

core::ActorUuid uuidOf(char fill) {
  core::ActorUuid u;
  std::memset(u.data(), fill, 32);
  return u;
}

std::vector<std::uint8_t> makeNotification(wire::MessageType type, const core::ActorUuid& uuid,
                                           const wire::ChunkCoord& chunk, Bytes payload,
                                           std::int64_t epochMs, std::uint8_t seq) {
  wire::LongSpatialParams p;
  p.type = type;
  p.appId = 7;
  p.chunk = chunk;
  p.distance = 8;
  p.uuid = uuid;
  p.payload = payload;
  p.gameTokenId = epochMs;
  p.sequence = seq;
  std::vector<std::uint8_t> buf(wire::longSpatialSize(payload.size()));
  auto n = wire::encodeLongSpatial(core::opensslCrypto(), p, token64(),
                                   MutableBytes(buf.data(), buf.size()));
  CHECK(n.ok());
  return buf;
}

void run() {
  FakeServer server;
  server.start();

  Config cfg;
  cfg.appId = 7;
  cfg.token = TokenInfo{kToken, 42, 0};
  cfg.manualPump = true;
  cfg.sessionReadyWaitMs = 0;
  auto conn = std::make_shared<Connection>(cfg, std::make_shared<StubProvider>(server.port));
  CHECK(conn->connect().ok());

  WorldSessionConfig sess;
  sess.appId = "7";
  sess.self.sendHz = 1000;  // effectively every tick
  sess.self.heartbeatIntervalMs = 0;
  sess.actors.staleAfterMs = 200;
  sess.actors.historySize = 2;
  sess.hostHeartbeatIntervalMs = 0;  // no CrowdyClient in this test
  sess.reapIntervalMs = 10;
  WorldSession session(conn, nullptr, sess);

  // --- Join sends the first actor update.
  const std::uint8_t pose[] = {9, 9, 9, 9};
  CHECK(session.join({0, 0, 0}, Bytes(pose, sizeof(pose))).ok());
  auto joinMsg = server.recvOne();
  CHECK_EQ(joinMsg[0], 128u);
  auto parsedJoin = wire::parseLongSpatial(Bytes(joinMsg.data(), joinMsg.size()));
  CHECK(parsedJoin.ok());
  CHECK(std::memcmp(parsedJoin->uuid, session.actorUuid().data(), 32) == 0);

  // --- Send-on-change: unchanged state does not resend before the keyframe.
  session.tick();
  session.self().setState(Bytes(pose, sizeof(pose)));  // identical -> no dirty
  // Changed state resends.
  const std::uint8_t pose2[] = {1, 2, 3, 4};
  session.self().setState(Bytes(pose2, sizeof(pose2)));
  for (int i = 0; i < 10; ++i) session.tick();
  auto changed = server.recvOne();
  CHECK_EQ(changed[0], 128u);

  // --- Remote actor ingest: another player's update lands in the registry.
  const auto other = uuidOf('z');
  const std::uint8_t otherPose[] = {5, 5};
  auto note = makeNotification(wire::MessageType::ActorUpdateNotification, other, {1, 0, 0},
                               Bytes(otherPose, sizeof(otherPose)), 1700000000000LL, 1);
  server.reply(note.data(), note.size());
  auto note2 = makeNotification(wire::MessageType::ActorUpdateNotification, other, {1, 0, 0},
                                Bytes(otherPose, sizeof(otherPose)), 1700000000100LL, 2);
  server.reply(note2.data(), note2.size());

  // Self-echo must be filtered out of the remote registry.
  auto selfNote = makeNotification(wire::MessageType::ActorUpdateNotification,
                                   session.actorUuid(), {0, 0, 0},
                                   Bytes(pose, sizeof(pose)), 1700000000200LL, 3);
  server.reply(selfNote.data(), selfNote.size());

  for (int i = 0; i < 100 && session.actors().size() < 1; ++i) {
    conn->pump(20);
    session.tick();
  }
  CHECK_EQ(session.actors().size(), 1u);
  const RemoteActor* remote = session.actors().find(other);
  CHECK(remote != nullptr);
  CHECK_EQ(remote->chunk.x, 1);
  CHECK_EQ(remote->samples.size(), 2u);                          // history kept
  CHECK_EQ(remote->lastServerEpochMs, 1700000000100LL);          // newest first
  CHECK_EQ(remote->state().size(), 2u);

  // --- Voxel merge into the chunk cache.
  std::uint8_t voxelPayload[wire::voxel::kFixedSize + 2];
  const std::uint8_t meta[] = {0xca, 0xfe};
  wire::encodeVoxelPayload(3, 4, 5, 7, Bytes(meta, sizeof(meta)),
                           MutableBytes(voxelPayload, sizeof(voxelPayload)));
  auto voxelNote = makeNotification(wire::MessageType::VoxelUpdateNotification, other, {2, 0, 0},
                                    Bytes(voxelPayload, sizeof(voxelPayload)), 1700000000300LL, 4);
  server.reply(voxelNote.data(), voxelNote.size());

  // --- Channel + direct messages into inboxes.
  {
    wire::ChannelMessageParams cm;
    cm.channelId = 99;
    cm.uuid = other;
    const std::uint8_t hi[] = {'h', 'i'};
    cm.payload = Bytes(hi, sizeof(hi));
    cm.gameTokenId = 42;
    cm.sequence = 5;
    // Server->client channel notification layout differs from the request;
    // build it directly.
    std::uint8_t frame[128];
    frame[0] = 18;
    le::writeI64(frame + wire::channel::kChannelIdOffset, 99);
    std::memcpy(frame + wire::channel::kUuidOffset, other.data(), 32);
    le::writeU16(frame + wire::channel::kPayloadLenOffset, 2);
    frame[wire::channel::kPayloadOffset] = 'h';
    frame[wire::channel::kPayloadOffset + 1] = 'i';
    le::writeI64(frame + wire::channel::kPayloadOffset + 2, 1700000000400LL);
    frame[wire::channel::kPayloadOffset + 10] = 5;
    server.reply(frame, wire::channel::kPayloadOffset + 11);
  }
  auto direct = makeNotification(wire::MessageType::SingleActorMessage, session.actorUuid(),
                                 {0, 0, 0}, Bytes(reinterpret_cast<const std::uint8_t*>("dm"), 2),
                                 1700000000500LL, 6);
  server.reply(direct.data(), direct.size());

  // --- Error attribution.
  const std::uint8_t seqUsed = *session.self().lastSequence();
  const std::uint8_t errFrame[] = {3, seqUsed, 7};  // UNAUTHORIZED for our actor send
  server.reply(errFrame, sizeof(errFrame));

  for (int i = 0;
       i < 200 && (session.chunks().size() < 1 || session.channelInbox().size() < 1 ||
                   session.directInbox().size() < 1 || session.errors().recent().empty());
       ++i) {
    conn->pump(20);
    session.tick();
  }

  const ChunkData* chunk = session.chunks().find({2, 0, 0});
  CHECK(chunk != nullptr);
  CHECK_EQ(chunk->voxels[static_cast<std::size_t>(voxelIndex(3, 4, 5))], 7u);
  auto vs = chunk->voxelStates.find(voxelIndex(3, 4, 5));
  CHECK(vs != chunk->voxelStates.end());
  CHECK_EQ(vs->second.state.size(), 2u);

  auto channelMsgs = session.channelInbox().drain();
  CHECK_EQ(channelMsgs.size(), 1u);
  CHECK_EQ(channelMsgs[0].channelId, 99);
  CHECK_EQ(channelMsgs[0].payload.size(), 2u);

  auto dms = session.directInbox().drain();
  CHECK_EQ(dms.size(), 1u);
  CHECK_EQ(dms[0].payload.size(), 2u);

  CHECK(!session.errors().recent().empty());
  const AttributedError& err = session.errors().recent().back();
  CHECK_EQ(static_cast<int>(err.code), 7);
  CHECK_EQ(static_cast<int>(err.kind), static_cast<int>(SendKind::ActorUpdate));

  // --- Optimistic local voxel edit sends a VOXEL_UPDATE_REQUEST.
  const std::uint8_t stateBytes[] = {1};
  auto seq = session.chunks().setVoxel({2, 0, 0}, 1, 1, 1, 3, Bytes(stateBytes, 1),
                                       session.actorUuid());
  CHECK(seq.ok());
  const ChunkData* edited = session.chunks().find({2, 0, 0});
  CHECK_EQ(edited->voxels[static_cast<std::size_t>(voxelIndex(1, 1, 1))], 3u);
  // Drain until we see the voxel request (actor keyframes may interleave).
  bool sawVoxel = false;
  for (int i = 0; i < 5 && !sawVoxel; ++i) sawVoxel = server.recvOne()[0] == 131;
  CHECK(sawVoxel);

  // --- Staleness reaping: with no more traffic the remote actor expires.
  for (int i = 0; i < 100 && session.actors().size() > 0; ++i) {
    conn->pump(5);
    session.tick();
  }
  CHECK_EQ(session.actors().size(), 0u);

  session.dispose();
}

}  // namespace

int main() {
  run();
  std::puts("session_test OK");
  return 0;
}
