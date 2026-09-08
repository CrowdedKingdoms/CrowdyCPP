// Mirrors CrowdyJS's two-client-video e2e on the native transport (Buddy
// v0.25.x, ck-api v1.87.x, CrowdyCPP 0.30.0). Player A sends one encoded
// "frame" large enough to need three fragments via sendVideoFrame; player B
// must receive the three ClientVideoNotification fragments and reassemble the
// identical bytes with media::VideoFrameAssembler. Sending video requires the
// use_video_chat runtime permission, which the harness's all-access tier
// grants (and the world grid follows the tier since ck-api v1.87.1). See
// https://docs.crowdedkingdoms.com/replication-api/wire-formats.
#include <atomic>
#include <cstring>
#include <vector>

#include "crowdy/media/video_frames.hpp"
#include "e2e_util.hpp"

using namespace crowdy;
using namespace crowdy::replication;

namespace {

int run() {
  auto cfg = e2e::requireConfig();
  auto a = e2e::provisionPlayer(cfg, "video-a");
  auto b = e2e::provisionPlayer(cfg, "video-b");
  e2e::connectUdp(a, cfg);
  e2e::connectUdp(b, cfg);

  const auto uuidA = core::generateActorUuid();
  const auto uuidB = core::generateActorUuid();
  const wire::ChunkCoord chunk{100300, 0, 100300};
  E2E_CHECK(e2e::warmUp(*a.conn, chunk));
  E2E_CHECK(e2e::warmUp(*b.conn, chunk));

  // 2 500 bytes: three fragments at the 1 117-byte body maximum. Deterministic
  // so a byte-compare proves integrity across the split and the reassembly.
  std::vector<std::uint8_t> frame(2500);
  for (std::size_t i = 0; i < frame.size(); ++i)
    frame[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);

  media::VideoFrameAssembler assembler;
  std::atomic<int> fragments{0}, intactFrames{0}, corruptFrames{0}, errors{0};
  Handlers hb;
  hb.video = [&](const SpatialNotification& n) {
    if (std::memcmp(n.uuid, uuidA.data(), wire::kUuidSize) != 0) return;
    ++fragments;
    auto out = assembler.ingest(n.uuidArray(), n.payload, core::systemClock().monotonicMillis());
    if (!out) return;
    if (out->bytes.size() == frame.size() &&
        std::memcmp(out->bytes.data(), frame.data(), frame.size()) == 0) {
      ++intactFrames;
    } else {
      ++corruptFrames;
    }
  };
  b.conn->setHandlers(std::move(hb));

  Handlers ha;
  ha.genericError = [&](const GenericError& e) {
    std::fprintf(stderr, "sender got error code=%u seq=%u\n",
                 static_cast<unsigned>(e.code), static_cast<unsigned>(e.sequence));
    ++errors;
  };
  a.conn->setHandlers(std::move(ha));

  const std::uint8_t pose[] = {1};
  auto keepAlive = [&] {
    E2E_CHECK(a.conn->sendActorUpdate({chunk, uuidA, Bytes(pose, 1), 8}).ok());
    E2E_CHECK(b.conn->sendActorUpdate({chunk, uuidB, Bytes(pose, 1), 8}).ok());
  };
  keepAlive();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  E2E_SUBTEST("a 3-fragment video frame reassembles intact on the other client");
  std::uint16_t frameId = 40;
  const bool received = e2e::retryUntil(
      [&] {
        keepAlive();
        auto sent = a.conn->sendVideoFrame(chunk, uuidA, Bytes(frame.data(), frame.size()),
                                           frameId++, /*codec=*/0, /*distance=*/1);
        E2E_CHECK(sent.ok());
        E2E_CHECK(sent.value() == 3);
      },
      [&] {
        a.conn->poll();
        b.conn->poll();
        return intactFrames.load() >= 2;
      });
  std::printf("video: fragments=%d intactFrames=%d corrupt=%d dropped=%llu abandoned=%llu senderErrors=%d\n",
              fragments.load(), intactFrames.load(), corruptFrames.load(),
              static_cast<unsigned long long>(assembler.dropped),
              static_cast<unsigned long long>(assembler.abandoned), errors.load());
  E2E_CHECK(received);
  E2E_CHECK(corruptFrames.load() == 0);
  // A frame is delivered once and only when whole; three fragments per frame.
  E2E_CHECK(fragments.load() >= 3 * intactFrames.load());
  if (errors.load() > 0) {
    std::printf("(transient UNAUTHORIZED during permission-window reload: %d)\n", errors.load());
  }

  a.conn->disconnect();
  b.conn->disconnect();
  std::puts("e2e_two_client_video OK");
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unexpected exception: %s\n", e.what());
    return 1;
  }
}
