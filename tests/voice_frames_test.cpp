// The voice payload convention (CrowdyJS 18.7.0 / CrowdyCPP 0.60.0). The fixture is a copy of
// CrowdyJS's test/unit/fixtures/voice-frames.json (tests/parity/voice-frames-fixture.test.mjs holds
// the copy to the pinned commit's), replayed here exactly as CrowdyJS's voice-frames.test.mjs
// replays it: a change to either SDK's behaviour changes the fixture and both replays.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "crowdy/graphql/json.hpp"
#include "crowdy/media/voice_frames.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::media;

namespace {

std::vector<std::uint8_t> unhex(std::string_view text) {
  std::vector<std::uint8_t> out;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  CHECK(text.size() % 2 == 0);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1]);
    CHECK(hi >= 0 && lo >= 0);
    out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
  }
  return out;
}

std::string hex(std::span<const std::uint8_t> bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xf]);
  }
  return out;
}

graphql::Json fixture() {
  std::ifstream in(std::string(CROWDY_PARITY_FIXTURE_DIR) + "/voice-frames.json", std::ios::binary);
  CHECK(in.good());
  std::stringstream text;
  text << in.rdbuf();
  graphql::Json fx = graphql::Json::parse(text.str());
  CHECK(fx.ok());
  return fx;
}

VoiceHeader headerOf(const graphql::Json& h) {
  VoiceHeader header;
  header.codec = static_cast<std::uint8_t>(h["codec"].asInt64());
  header.seq = static_cast<std::uint16_t>(h["seq"].asInt64());
  header.timestamp = static_cast<std::uint32_t>(h["timestamp"].asInt64());
  header.frameMs = static_cast<std::uint8_t>(h["frameMs"].asInt64());
  header.flags = static_cast<std::uint8_t>(h["flags"].asInt64());
  return header;
}

bool sameHeader(const VoiceHeader& a, const graphql::Json& b) {
  return a.version == (b["version"].isNull() ? 1 : b["version"].asInt64()) &&
         a.codec == b["codec"].asInt64() && a.seq == b["seq"].asInt64() &&
         a.timestamp == b["timestamp"].asInt64() && a.frameMs == b["frameMs"].asInt64() &&
         a.flags == b["flags"].asInt64();
}

const char* resultName(VoicePushResult r) {
  switch (r) {
    case VoicePushResult::Buffered: return "buffered";
    case VoicePushResult::Late: return "late";
    case VoicePushResult::Duplicate: return "duplicate";
    case VoicePushResult::Malformed: return "malformed";
  }
  return "?";
}

void testConstants() {
  CHECK_EQ(kVoiceHeaderBytes, 10u);
  CHECK_EQ(kVoiceHeaderVersion, 1u);
  CHECK_EQ(kMaxVoiceFrameBytes, 1113u);
  CHECK_EQ(kVoiceTargetDelayMs, 60);
  CHECK_EQ(kVoiceJitterMaxFrames, 64);
  CHECK_EQ(kVoiceResetAfterMs, 200);
  CHECK_EQ(static_cast<int>(VoiceCodec::Raw), 0);
  CHECK_EQ(static_cast<int>(VoiceCodec::Opus), 1);
  CHECK_EQ(static_cast<int>(VoiceCodec::Mulaw), 2);
  CHECK_EQ(VoiceFlag::kSpurtStart, 1u);
  CHECK_EQ(VoiceFlag::kSpurtEnd, 2u);
  CHECK_EQ(voiceClockRate(1), 48000u);
  CHECK_EQ(voiceClockRate(2), 8000u);
  CHECK_EQ(voiceClockRate(0), 1000u);
  CHECK_EQ(voiceClockRate(9), 1000u);
  CHECK_EQ(voiceSamplesPerFrame(1, 20), 960u);
  CHECK_EQ(voiceSamplesPerFrame(2, 60), 480u);
  CHECK_EQ(voiceSeqDiff(0, 65535), 1);
  CHECK_EQ(voiceSeqDiff(65535, 0), -1);
  CHECK_EQ(voiceSeqDiff(0x8000, 0), -0x8000);
  CHECK_EQ(voiceSeqDiff(0x7fff, 0), 0x7fff);
}

void testFixtureHeaders(const graphql::Json& fx) {
  CHECK(fx["headers"].size() > 0);
  fx["headers"].forEach([](graphql::Json c) {
    const auto frame = unhex(c["frame"].asStringView());
    const auto packet = encodeVoicePacket(headerOf(c["header"]), frame);
    CHECK_EQ(hex(packet), c["packet"].asString());
    const auto head = encodeVoiceHeader(headerOf(c["header"]));
    CHECK_EQ(hex(head), c["packet"].asString().substr(0, 20));
    const auto bytes = unhex(c["packet"].asStringView());
    const auto decoded = decodeVoicePacket(bytes);
    CHECK(decoded.has_value());
    CHECK(sameHeader(decoded->header, c["header"]));
    CHECK_EQ(hex(decoded->frame), c["frame"].asString());
  });
  CHECK(fx["decoded"].size() > 0);
  fx["decoded"].forEach([](graphql::Json c) {
    const auto bytes = unhex(c["packet"].asStringView());
    const auto decoded = decodeVoicePacket(bytes);
    CHECK(decoded.has_value());
    CHECK(sameHeader(decoded->header, c["header"]));
    CHECK_EQ(hex(decoded->frame), c["frame"].asString());
  });
  CHECK(fx["rejected"].size() > 0);
  fx["rejected"].forEach([](graphql::Json c) {
    const auto bytes = unhex(c["packet"].asStringView());
    CHECK(!decodeVoicePacket(bytes).has_value());
  });
}

void testFixturePacketizer(const graphql::Json& fx) {
  CHECK(fx["packetizer"].size() > 0);
  fx["packetizer"].forEach([](graphql::Json c) {
    VoicePacketizerOptions options;
    options.codec = static_cast<std::uint8_t>(c["options"]["codec"].asInt64());
    options.frameMs = static_cast<std::uint8_t>(c["options"]["frameMs"].asInt64());
    options.seq = static_cast<std::uint16_t>(c["options"]["seq"].asInt64(0));
    options.timestamp = static_cast<std::uint32_t>(c["options"]["timestamp"].asInt64(0));
    VoicePacketizer packetizer(options);
    c["steps"].forEach([&](graphql::Json step) {
      if (!step["skip"].isNull()) {
        packetizer.skip(static_cast<std::uint32_t>(step["skip"].asInt64()));
        CHECK_EQ(packetizer.nextSeq(), step["nextSeq"].asInt64());
        CHECK_EQ(packetizer.nextTimestamp(), step["nextTimestamp"].asInt64());
      } else {
        const auto frame = unhex(step["packetize"].asStringView());
        const auto packet = packetizer.packetize(frame, step["last"].asBool(false));
        CHECK_EQ(hex(packet), step["packet"].asString());
      }
    });
  });
}

void testFixtureJitter(const graphql::Json& fx) {
  CHECK(fx["jitter"].size() > 0);
  fx["jitter"].forEach([](graphql::Json scenario) {
    VoiceJitterBufferOptions options;
    options.targetDelayMs = scenario["options"]["targetDelayMs"].asInt64();
    options.maxFrames = static_cast<int>(scenario["options"]["maxFrames"].asInt64());
    options.resetAfterMs = scenario["options"]["resetAfterMs"].asInt64();
    VoiceJitterBuffer buffer(options);
    scenario["events"].forEach([&](graphql::Json event) {
      const std::int64_t at = event["at"].asInt64();
      if (event["push"].isString()) {
        const std::string key = event["push"].asString();
        const auto packet = event["packet"].isString()
                                ? unhex(event["packet"].asStringView())
                                : encodeVoicePacket(headerOf(event["header"]),
                                                    unhex(event["frame"].asStringView()));
        const VoicePushResult result = buffer.push(key, packet, at);
        if (std::string(resultName(result)) != event["result"].asString()) {
          std::fprintf(stderr, "push at %lld: %s, expected %s\n", static_cast<long long>(at),
                       resultName(result), event["result"].asString().c_str());
          CHECK(false);
        }
        CHECK_EQ(buffer.bufferedCount(key), static_cast<std::size_t>(event["buffered"].asInt64()));
      } else if (event["pull"].isString()) {
        const std::string key = event["pull"].asString();
        const auto played = buffer.pull(key, at);
        if (played.size() != event["expect"].size()) {
          std::fprintf(stderr, "pull at %lld: %zu slots, expected %zu\n",
                       static_cast<long long>(at), played.size(), event["expect"].size());
          CHECK(false);
        }
        for (std::size_t i = 0; i < played.size(); ++i) {
          const VoicePlayout& slot = played[i];
          const graphql::Json want = event["expect"].at(i);
          CHECK_EQ(slot.key, key);
          CHECK_EQ(slot.seq, want["seq"].asInt64());
          CHECK_EQ(slot.timestamp, want["timestamp"].asInt64());
          CHECK_EQ(slot.codec, want["codec"].asInt64());
          CHECK_EQ(slot.frameMs, want["frameMs"].asInt64());
          CHECK_EQ(slot.flags, want["flags"].asInt64());
          CHECK_EQ(slot.gap, want["frame"].isNull());
          if (!slot.gap) CHECK_EQ(hex(slot.frame), want["frame"].asString());
          if (slot.gap) CHECK(slot.frame.empty());
        }
        CHECK_EQ(buffer.senderCount(), static_cast<std::size_t>(event["senders"].asInt64()));
      } else if (event["forget"].isString()) {
        buffer.forget(event["forget"].asString());
        CHECK_EQ(buffer.senderCount(), static_cast<std::size_t>(event["senders"].asInt64()));
      } else {
        CHECK(false);  // an event this replay does not know
      }
    });
    const graphql::Json counters = scenario["counters"];
    CHECK_EQ(buffer.late, static_cast<std::uint64_t>(counters["late"].asInt64()));
    CHECK_EQ(buffer.duplicates, static_cast<std::uint64_t>(counters["duplicates"].asInt64()));
    CHECK_EQ(buffer.malformed, static_cast<std::uint64_t>(counters["malformed"].asInt64()));
    CHECK_EQ(buffer.discarded, static_cast<std::uint64_t>(counters["discarded"].asInt64()));
    CHECK_EQ(buffer.gaps, static_cast<std::uint64_t>(counters["gaps"].asInt64()));
  });
}

void testWritersRefuseFramesThatDoNotFit() {
  VoiceHeader header;
  header.codec = 1;
  header.frameMs = 20;
  CHECK_EQ(encodeVoicePacket(header, std::vector<std::uint8_t>(1113)).size(), 1123u);
  CHECK(encodeVoicePacket(header, std::vector<std::uint8_t>(1114)).empty());
  VoicePacketizer packetizer({1, 20, 0, 0});
  CHECK(packetizer.packetize(std::vector<std::uint8_t>(1114)).empty());
  CHECK_EQ(packetizer.nextSeq(), 0u);  // a refused frame is not numbered
  VoicePacketizer zero({1, 0, 0, 0});
  CHECK(zero.packetize(std::vector<std::uint8_t>(3)).empty());
  // Out-of-range options are clamped rather than refused (CrowdyJS throws RangeError).
  VoiceJitterBuffer clamped({-5, 0, 0});
  const std::vector<std::uint8_t> one = encodeVoicePacket({1, 2, 7, 0, 20, 1}, std::vector<std::uint8_t>{9});
  CHECK(clamped.push("k", one, 100) == VoicePushResult::Buffered);
  const auto now = clamped.pull("k", 100);  // a delay of 0: due at once
  CHECK_EQ(now.size(), 1u);
}

void testReadersNeverTrustThePacket() {
  VoiceJitterBuffer buffer;
  std::uint32_t seed = 7;
  auto next = [&seed] { return seed = seed * 1103515245u + 12345u; };
  for (int i = 0; i < 2000; ++i) {
    std::vector<std::uint8_t> bytes((next() >> 8) % 24);
    for (auto& b : bytes) b = static_cast<std::uint8_t>(next() >> 16);
    if (i % 3 == 0 && !bytes.empty()) bytes[0] = 1;
    (void)decodeVoicePacket(bytes);
    (void)buffer.push("k" + std::to_string(i % 5), bytes, i);
    (void)buffer.poll(i);
  }
  CHECK(buffer.malformed > 0);
  for (int k = 0; k < 5; ++k) CHECK(buffer.bufferedCount("k" + std::to_string(k)) <= 64u);
}

void testASpeakerPlaysBackInOrder() {
  VoicePacketizer packetizer({static_cast<std::uint8_t>(VoiceCodec::Opus), 20, 65530, 0});
  VoiceJitterBuffer buffer;
  struct Arrival {
    std::int64_t at;
    std::vector<std::uint8_t> packet;
  };
  std::vector<Arrival> arrivals;
  for (int i = 0; i < 10; ++i) {
    const std::uint8_t frame[] = {static_cast<std::uint8_t>(i)};
    arrivals.push_back({i * 20 + (i % 4 == 1 ? 25 : 0), packetizer.packetize(frame, i == 9)});
  }
  std::vector<VoicePlayout> played;
  for (std::int64_t t = 0; t <= 400; ++t) {
    for (const Arrival& a : arrivals) {
      if (a.at == t) CHECK(buffer.push("speaker", a.packet, t) == VoicePushResult::Buffered);
    }
    for (auto& slot : buffer.pull("speaker", t)) played.push_back(std::move(slot));
  }
  CHECK_EQ(played.size(), 10u);
  for (std::size_t i = 0; i < played.size(); ++i) {
    CHECK(!played[i].gap);
    CHECK_EQ(played[i].frame.at(0), static_cast<std::uint8_t>(i));
    CHECK_EQ(played[i].seq, static_cast<std::uint16_t>(65530 + i));
  }
  CHECK_EQ(played.front().flags, VoiceFlag::kSpurtStart);
  CHECK_EQ(played.back().flags, VoiceFlag::kSpurtEnd);
  CHECK_EQ(buffer.gaps, 0u);
  CHECK_EQ(buffer.late, 0u);
}

}  // namespace

int main() {
  const graphql::Json fx = fixture();
  testConstants();
  testFixtureHeaders(fx);
  testFixturePacketizer(fx);
  testFixtureJitter(fx);
  testWritersRefuseFramesThatDoNotFit();
  testReadersNeverTrustThePacket();
  testASpeakerPlaysBackInOrder();
  std::puts("voice_frames_test OK");
  return 0;
}
