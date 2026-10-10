// The optional libopus wrapper (CROWDY_WITH_OPUS=ON only): 48 kHz mono frames through the voice
// convention, a lost frame concealed, and the refusals.
#include <cmath>
#include <cstdio>
#include <vector>

#include "crowdy/media/opus.hpp"
#include "crowdy/media/voice_frames.hpp"
#include "test_util.hpp"

using namespace crowdy;
using namespace crowdy::media;

static_assert(kOpusAvailable, "opus_test is built only with CROWDY_WITH_OPUS=ON");

namespace {

std::vector<std::int16_t> tone(std::size_t samples, std::size_t offset) {
  std::vector<std::int16_t> pcm(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const double t = static_cast<double>(offset + i) / 48000.0;
    pcm[i] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
  }
  return pcm;
}

double rms(const std::vector<std::int16_t>& pcm) {
  double sum = 0;
  for (std::int16_t s : pcm) sum += static_cast<double>(s) * s;
  return pcm.empty() ? 0 : std::sqrt(sum / static_cast<double>(pcm.size()));
}

void testRefusals() {
  CHECK(OpusVoiceEncoder::create({25, 24000, 0}).error() == Errc::InvalidArgument);
  CHECK(OpusVoiceEncoder::create({20, 1000, 0}).error() == Errc::InvalidArgument);
  CHECK(OpusVoiceEncoder::create({20, 24000, 101}).error() == Errc::InvalidArgument);
  auto encoder = OpusVoiceEncoder::create();
  CHECK(encoder.ok());
  CHECK_EQ(encoder->frameSamples(), 960u);
  CHECK(encoder->encode(tone(959, 0)).error() == Errc::InvalidArgument);
  auto decoder = OpusVoiceDecoder::create();
  CHECK(decoder.ok());
  CHECK(decoder->decode({}).error() == Errc::Malformed);
  CHECK(decoder->conceal(0).error() == Errc::InvalidArgument);
  CHECK(decoder->conceal(7).error() == Errc::InvalidArgument);
}

// A sender encodes ten 20 ms frames and packetizes them; one is lost on the way. The receiver's
// jitter buffer plays nine frames and one gap, which the decoder conceals: 200 ms of audio.
void testThroughTheVoiceConvention() {
  auto encoder = OpusVoiceEncoder::create({20, 32000, 10});
  auto decoder = OpusVoiceDecoder::create();
  CHECK(encoder.ok() && decoder.ok());
  VoicePacketizer packetizer({static_cast<std::uint8_t>(VoiceCodec::Opus), 20, 0, 0});
  VoiceJitterBuffer buffer;
  for (int i = 0; i < 10; ++i) {
    auto frame = encoder->encode(tone(960, static_cast<std::size_t>(i) * 960));
    CHECK(frame.ok());
    CHECK(!frame->empty() && frame->size() <= kMaxVoiceFrameBytes);
    const auto packet = packetizer.packetize(frame.value(), i == 9);
    if (i == 4) continue;  // lost
    CHECK(buffer.push("speaker", packet, i * 20) == VoicePushResult::Buffered);
  }
  std::vector<std::int16_t> out;
  int concealed = 0;
  for (std::int64_t t = 0; t <= 400; t += 5) {
    for (const VoicePlayout& slot : buffer.pull("speaker", t)) {
      CHECK_EQ(slot.codec, static_cast<std::uint8_t>(VoiceCodec::Opus));
      auto pcm = slot.gap ? decoder->conceal(slot.frameMs) : decoder->decode(slot.frame);
      CHECK(pcm.ok());
      CHECK_EQ(pcm->size(), 960u);
      if (slot.gap) ++concealed;
      out.insert(out.end(), pcm->begin(), pcm->end());
    }
  }
  CHECK_EQ(concealed, 1);
  CHECK_EQ(out.size(), 9600u);
  const double level = rms(out);
  CHECK(level > 2000.0 && level < 9000.0);  // the tone came through (its RMS is ~5657)
}

}  // namespace

int main() {
  testRefusals();
  testThroughTheVoiceConvention();
  std::puts("opus_test OK");
  return 0;
}
