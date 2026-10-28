#include "decoder.h"
#include "media_fixture.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace pb = media::pb;
void verify(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
float sample(const std::string& bytes, size_t offset) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  return std::bit_cast<float>(value);
}
struct Result {
  std::vector<pb::StreamEvent> events;
  size_t read_at_first_audio = 0;
};
Result decode(const std::string& bytes, pb::DecodeOptions options, size_t read_size = 777) {
  Result result;
  size_t offset = 0;
  media::decode_media([&](uint8_t* output, int capacity) {
    const auto count = std::min({read_size, bytes.size() - offset, static_cast<size_t>(capacity)});
    std::memcpy(output, bytes.data() + offset, count);
    offset += count;
    return static_cast<int>(count);
  }, [&](pb::StreamEvent event) {
    if (event.has_audio() && !result.read_at_first_audio) result.read_at_first_audio = offset;
    result.events.push_back(std::move(event));
  }, [] { return false; }, options);
  return result;
}
pb::DecodeOptions all() {
  pb::DecodeOptions options;
  options.set_emit_audio(true);
  options.set_emit_packets(true);
  return options;
}
void wave_live_and_exact_samples() {
  const auto bytes = wav_fixture();
  const auto result = decode(bytes, all());
  std::cout << "WAV first audio after " << result.read_at_first_audio << "/" << bytes.size() << " source bytes\n";
  verify(result.read_at_first_audio > 0 && result.read_at_first_audio < bytes.size(), "decoder buffered WAV to EOF");
  uint64_t samples = 0, chunks = 0;
  bool metadata = false;
  for (const auto& event : result.events) {
    if (event.has_media_info()) {
      metadata = true;
      verify(event.media_info().tracks_size() == 1, "WAV track missing");
      verify(event.media_info().tracks(0).codec_name() == "pcm_s16le", "wrong WAV codec");
    }
    if (!event.has_audio()) continue;
    const auto& audio = event.audio();
    verify(audio.first_sample() == samples && audio.chunk_number() == ++chunks, "audio sequence gap");
    verify(audio.channels() == 1 && audio.sample_rate() == 16000, "wrong output audio format");
    for (size_t i = 0; i < audio.pcm_f32le().size(); i += 4) {
      const float expected = (static_cast<int>(samples % 2000) - 1000) / 32768.0f;
      verify(std::abs(sample(audio.pcm_f32le(), i) - expected) < 1e-7, "PCM sample changed");
      ++samples;
    }
  }
  verify(metadata && samples == 48000, "WAV lost samples or metadata");
}
void resample_stereo() {
  const auto result = decode(wav_fixture(48000, 2), all());
  uint64_t samples = 0;
  for (const auto& event : result.events) if (event.has_audio()) {
    verify(event.audio().first_sample() == samples, "resampling timeline gap");
    samples += event.audio().pcm_f32le().size() / 4;
  }
  verify(samples == 48000, "resampler tail missing or duplicated");
}
void video_and_audio() {
  const auto bytes = nut_fixture();
  const auto result = decode(bytes, all());
  std::cout << "NUT first audio after " << result.read_at_first_audio << "/" << bytes.size() << " source bytes\n";
  verify(result.read_at_first_audio > 0 && result.read_at_first_audio < bytes.size(), "video demux buffered to EOF");
  int video_packets = 0, audio_packets = 0;
  uint64_t samples = 0;
  bool title = false, video = false;
  for (const auto& event : result.events) {
    if (event.has_media_info()) {
      for (const auto& field : event.media_info().metadata()) title |= field.key() == "title" && field.value() == "Live fixture";
      for (const auto& track : event.media_info().tracks())
        video |= track.kind() == pb::TRACK_KIND_VIDEO && track.width() == 4 && track.height() == 4;
    }
    if (event.has_packet()) {
      const auto& packet = event.packet();
      verify(packet.has_pts() && packet.has_dts() && packet.time_base().denominator() != 0, "packet timing missing");
      verify(packet.fragment_offset() == 0 && packet.data().size() == packet.packet_bytes(), "small packet fragmented incorrectly");
      if (packet.track_id() == 1) {
        verify(packet.data() == std::string(48, static_cast<char>(video_packets)), "video packet changed");
        ++video_packets;
      } else { ++audio_packets; }
    }
    if (event.has_audio()) samples += event.audio().pcm_f32le().size() / 4;
  }
  verify(title && video, "container or video metadata missing");
  verify(video_packets == 30 && audio_packets == 30 && samples == 48000, "audio/video content lost");
}
void malformed_and_cancelled() {
  bool failed = false;
  try { decode("not a media container", all()); }
  catch (const media::Failure& e) { failed = e.code == grpc::StatusCode::INVALID_ARGUMENT; }
  verify(failed, "malformed media accepted");
  failed = false;
  try { media::decode_media([](uint8_t*, int) { return 0; }, [](pb::StreamEvent) {}, [] { return true; }, all()); }
  catch (const media::Failure& e) { failed = e.code == grpc::StatusCode::CANCELLED; }
  verify(failed, "decode ignored cancellation");
  failed = false;
  try { media::decode_media([](uint8_t*, int) -> int { throw media::Failure(grpc::StatusCode::DATA_LOSS, "source failed"); }, [](pb::StreamEvent) {}, [] { return false; }, all()); }
  catch (const media::Failure& e) { failed = e.code == grpc::StatusCode::DATA_LOSS; }
  verify(failed, "C callback lost original input error");
  auto options = all();
  options.add_audio_track_ids(9);
  failed = false;
  try { decode(wav_fixture(), options); }
  catch (const media::Failure& e) { failed = e.code == grpc::StatusCode::INVALID_ARGUMENT; }
  verify(failed, "missing selected track accepted");
  auto truncated = wav_fixture();
  truncated.resize(truncated.size() - 10000);
  failed = false;
  try { decode(truncated, all()); }
  catch (const media::Failure& e) { failed = e.code == grpc::StatusCode::INVALID_ARGUMENT; }
  verify(failed, "truncated PCM WAV reported success");
}

void packet_fragmentation() {
  const auto result = decode(nut_fixture(256, 256, 1), all());
  size_t offset = 0;
  unsigned fragments = 0;
  uint64_t packet_number = 0;
  for (const auto& event : result.events) if (event.has_packet() && event.packet().track_id() == 1) {
    const auto& packet = event.packet();
    if (!packet_number) packet_number = packet.packet_number();
    verify(packet.packet_number() == packet_number, "packet fragments changed packet identity");
    verify(packet.fragment_offset() == offset, "packet fragment gap");
    verify(packet.packet_bytes() == 256 * 256 * 3, "packet total size changed");
    verify(packet.data().size() <= 128 * 1024, "unbounded packet message");
    verify(packet.data() == std::string(packet.data().size(), '\0'), "packet fragment changed bytes");
    offset += packet.data().size();
    ++fragments;
  }
  verify(fragments == 2 && offset == 256 * 256 * 3, "packet fragmentation lost payload");
}

int main() {
  try {
    wave_live_and_exact_samples();
    resample_stereo();
    video_and_audio();
    malformed_and_cancelled();
    packet_fragmentation();
    std::cout << "5 native media tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
