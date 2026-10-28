#include "media_fixture.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cstdint>
#include <exception>
#include <stdexcept>

namespace {
void u16(std::string& out, uint16_t value) {
  out += static_cast<char>(value & 255);
  out += static_cast<char>(value >> 8);
}
void u32(std::string& out, uint32_t value) {
  u16(out, value & 65535);
  u16(out, value >> 16);
}
void require(int result) {
  if (result < 0) throw std::runtime_error("native fixture mux failed: " + std::to_string(result));
}
struct Mux {
  AVFormatContext* format = nullptr;
  AVIOContext* io = nullptr;
  AVPacket* packet = nullptr;
  std::string bytes;
  std::exception_ptr failure;
  ~Mux() {
    av_packet_free(&packet);
    avformat_free_context(format);
    if (io) av_freep(&io->buffer);
    avio_context_free(&io);
  }
  static int write(void* opaque, const uint8_t* data, int length) noexcept {
    auto& self = *static_cast<Mux*>(opaque);
    try { self.bytes.append(reinterpret_cast<const char*>(data), length); return length; }
    catch (...) { self.failure = std::current_exception(); return AVERROR(ENOMEM); }
  }
};
}  // namespace

std::string wav_fixture(unsigned sample_rate, unsigned channels, unsigned seconds) {
  const unsigned frames = sample_rate * seconds;
  const unsigned data_bytes = frames * channels * 2;
  std::string result = "RIFF";
  u32(result, 36 + data_bytes);
  result += "WAVEfmt ";
  u32(result, 16);
  u16(result, 1);
  u16(result, channels);
  u32(result, sample_rate);
  u32(result, sample_rate * channels * 2);
  u16(result, channels * 2);
  u16(result, 16);
  result += "data";
  u32(result, data_bytes);
  for (unsigned i = 0; i < frames; ++i) for (unsigned channel = 0; channel < channels; ++channel)
    u16(result, static_cast<int16_t>(static_cast<int>(i % 2000) - 1000));
  return result;
}

std::string nut_fixture(unsigned width, unsigned height, unsigned frames) {
  Mux mux;
  require(avformat_alloc_output_context2(&mux.format, nullptr, "nut", nullptr));
  auto* buffer = static_cast<unsigned char*>(av_malloc(4096));
  if (!buffer) throw std::bad_alloc();
  // FFmpeg 7 changed the output callback's buffer to const.
#if LIBAVFORMAT_VERSION_MAJOR >= 61
  mux.io = avio_alloc_context(buffer, 4096, 1, &mux, nullptr, &Mux::write, nullptr);
#else
  mux.io = avio_alloc_context(buffer, 4096, 1, &mux, nullptr,
      [](void* opaque, uint8_t* data, int size) { return Mux::write(opaque, data, size); }, nullptr);
#endif
  if (!mux.io) { av_free(buffer); throw std::bad_alloc(); }
  mux.format->pb = mux.io;
  mux.format->flags |= AVFMT_FLAG_CUSTOM_IO;
  av_dict_set(&mux.format->metadata, "title", "Live fixture", 0);
  auto* audio = avformat_new_stream(mux.format, nullptr);
  auto* video = avformat_new_stream(mux.format, nullptr);
  if (!audio || !video) throw std::bad_alloc();
  audio->time_base = {1, 16000};
  audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
  audio->codecpar->sample_rate = 16000;
  audio->codecpar->bits_per_coded_sample = 16;
  audio->codecpar->block_align = 2;
  av_channel_layout_default(&audio->codecpar->ch_layout, 1);
  av_dict_set(&audio->metadata, "language", "eng", 0);
  video->time_base = {1, 10};
  video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  video->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
  video->codecpar->format = AV_PIX_FMT_RGB24;
  video->codecpar->width = width;
  video->codecpar->height = height;
  require(avformat_write_header(mux.format, nullptr));
  mux.packet = av_packet_alloc();
  if (!mux.packet) throw std::bad_alloc();
  for (unsigned index = 0; index < frames; ++index) {
    require(av_new_packet(mux.packet, 3200));
    mux.packet->stream_index = audio->index;
    mux.packet->pts = mux.packet->dts = av_rescale_q(index * 1600, {1, 16000}, audio->time_base);
    mux.packet->duration = av_rescale_q(1600, {1, 16000}, audio->time_base);
    mux.packet->flags = AV_PKT_FLAG_KEY;
    for (int sample = 0; sample < 1600; ++sample) {
      mux.packet->data[sample * 2] = 0;
      mux.packet->data[sample * 2 + 1] = 32;
    }
    require(av_interleaved_write_frame(mux.format, mux.packet));
    require(av_new_packet(mux.packet, width * height * 3));
    mux.packet->stream_index = video->index;
    mux.packet->pts = mux.packet->dts = av_rescale_q(index, {1, 10}, video->time_base);
    mux.packet->duration = av_rescale_q(1, {1, 10}, video->time_base);
    mux.packet->flags = AV_PKT_FLAG_KEY;
    std::fill_n(mux.packet->data, width * height * 3, static_cast<uint8_t>(index));
    require(av_interleaved_write_frame(mux.format, mux.packet));
  }
  require(av_write_trailer(mux.format));
  avio_flush(mux.io);
  if (mux.failure) std::rethrow_exception(mux.failure);
  return mux.bytes;
}
