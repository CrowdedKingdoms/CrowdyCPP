// The SDK-side video fragment contract (Buddy v0.25.0 / CrowdyJS 15.6.0).
// These seven cases mirror CrowdyJS test/unit/video-frames.test.mjs one for one;
// a change to either SDK's behaviour must change both files.
#include <cstdio>
#include <vector>

#include "crowdy/media/video_frames.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::media;

namespace {

std::vector<std::uint8_t> frame(std::size_t n, std::uint32_t seed = 7) {
  std::vector<std::uint8_t> f(n);
  for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<std::uint8_t>((i * seed + 3) & 0xff);
  return f;
}

core::ActorUuid uuidOf(char c) {
  core::ActorUuid u;
  u.fill(c);
  return u;
}

bool sameBytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

void testConstants() {
  CHECK_EQ(kVideoFragmentHeaderBytes, 6u);
  CHECK_EQ(kMaxVideoFragmentBodyBytes, 1117u);
  CHECK_EQ(kMaxVideoFragments, 16u);
  CHECK_EQ(kVideoFrameTimeoutMs, 500);
  CHECK_EQ(static_cast<int>(VideoCodec::Jpeg), 0);
  CHECK_EQ(static_cast<int>(VideoCodec::WebP), 1);
}

void testOneFragmentRoundTrip() {
  const auto f = frame(300);
  const auto packets = fragmentFrame(f, 0x1234, VideoCodec::Jpeg);
  CHECK_EQ(packets.size(), 1u);
  const std::uint8_t header[] = {1, 0, 0x12, 0x34, 0, 1};
  CHECK(std::equal(header, header + 6, packets[0].begin()));
  CHECK(std::equal(f.begin(), f.end(), packets[0].begin() + 6));
  VideoFrameAssembler a;
  const auto out = a.ingest(uuidOf('u'), packets[0], 1000);
  CHECK(out.has_value());
  CHECK_EQ(out->frameId, 0x1234u);
  CHECK(out->codec == VideoCodec::Jpeg);
  CHECK(sameBytes(out->bytes, f));
  CHECK_EQ(a.pendingCount(), 0u);
}

void testThreeFragmentsInOrder() {
  const auto f = frame(1117 * 2 + 100);
  const auto packets = fragmentFrame(f, 5, VideoCodec::WebP);
  CHECK_EQ(packets.size(), 3u);
  CHECK_EQ(packets[0].size() - 6, 1117u);
  CHECK_EQ(packets[1].size() - 6, 1117u);
  CHECK_EQ(packets[2].size() - 6, 100u);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK_EQ(packets[i][4], i);
    CHECK_EQ(packets[i][5], 3u);
    CHECK_EQ(packets[i][1], 1u);
  }
  VideoFrameAssembler a;
  const auto u = uuidOf('u');
  CHECK(!a.ingest(u, packets[0], 1).has_value());
  CHECK(!a.ingest(u, packets[1], 2).has_value());
  const auto out = a.ingest(u, packets[2], 3);
  CHECK(out.has_value());
  CHECK(sameBytes(out->bytes, f));
  CHECK_EQ(out->completedAtMs, 3);
}

void testOutOfOrderAndDuplicate() {
  const auto f = frame(2500, 11);
  const auto packets = fragmentFrame(f, 9);
  VideoFrameAssembler a;
  const auto u = uuidOf('u');
  CHECK(!a.ingest(u, packets[2], 1).has_value());
  CHECK(!a.ingest(u, packets[0], 2).has_value());
  CHECK(!a.ingest(u, packets[0], 3).has_value());  // duplicate
  const auto out = a.ingest(u, packets[1], 4);
  CHECK(out.has_value());
  CHECK(sameBytes(out->bytes, f));
  // The frame is done; a late duplicate is a straggler, dropped, not re-delivered.
  CHECK(!a.ingest(u, packets[1], 5).has_value());
  CHECK_EQ(a.dropped, 1u);
}

void testNewerFrameAbandonsOlderDropped() {
  VideoFrameAssembler a;
  const auto u = uuidOf('u');
  const auto old = fragmentFrame(frame(2000), 100);
  const auto newer = fragmentFrame(frame(300), 101);
  CHECK(!a.ingest(u, old[0], 1).has_value());
  const auto out = a.ingest(u, newer[0], 2);
  CHECK(out.has_value());  // the newer single-fragment frame completes
  CHECK_EQ(a.abandoned, 1u);
  CHECK(!a.ingest(u, old[1], 3).has_value());  // the rest of the old frame is a straggler
  CHECK_EQ(a.dropped, 1u);
  CHECK(isNewerFrameId(0, 65535));
  CHECK(!isNewerFrameId(65535, 0));
  CHECK(!isNewerFrameId(5, 5));
}

void testTimeoutAndForget() {
  VideoFrameAssembler a(500);
  const auto p = fragmentFrame(frame(2000), 1);
  const auto u = uuidOf('u');
  a.ingest(u, p[0], 1000);
  CHECK_EQ(a.prune(1400), 0u);
  CHECK_EQ(a.prune(1501), 1u);
  CHECK_EQ(a.pendingCount(), 0u);
  const auto v = uuidOf('v');
  a.ingest(v, p[0], 2000);
  a.forget(v);
  CHECK_EQ(a.pendingCount(), 0u);
  CHECK_EQ(a.abandoned, 2u);
}

void testMalformedHeaders() {
  VideoFrameAssembler a;
  const auto good = fragmentFrame(frame(100), 3)[0];
  const auto u = uuidOf('u');
  auto mutated = [&](auto fn) {
    auto bad = good;
    fn(bad);
    CHECK(!parseVideoFragmentHeader(bad).has_value());
    CHECK(!a.ingest(u, bad, 1).has_value());
  };
  mutated([](std::vector<std::uint8_t>& p) { p[0] = 2; });   // version
  mutated([](std::vector<std::uint8_t>& p) { p[1] = 9; });   // reserved codec
  mutated([](std::vector<std::uint8_t>& p) { p[5] = 0; });   // count 0
  mutated([](std::vector<std::uint8_t>& p) { p[5] = 17; });  // count > 16
  mutated([](std::vector<std::uint8_t>& p) { p[4] = 1; p[5] = 1; });  // index past count
  CHECK_EQ(a.dropped, 5u);
  const std::uint8_t five[5] = {1, 0, 0, 0, 0};
  CHECK(!parseVideoFragmentHeader(std::span<const std::uint8_t>(five, 5)).has_value());
}

void testFragmentFrameRefusals() {
  CHECK(fragmentFrame(frame(1117 * 16 + 1), 1).empty());  // 17 fragments
  CHECK_EQ(fragmentFrame(frame(1117 * 16), 1).size(), 16u);
  CHECK(fragmentFrame(std::vector<std::uint8_t>{}, 1).empty());
}

}  // namespace

int main() {
  testConstants();
  testOneFragmentRoundTrip();
  testThreeFragmentsInOrder();
  testOutOfOrderAndDuplicate();
  testNewerFrameAbandonsOlderDropped();
  testTimeoutAndForget();
  testMalformedHeaders();
  testFragmentFrameRefusals();
  std::puts("video_frames_test OK");
  return 0;
}
