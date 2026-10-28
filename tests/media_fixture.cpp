#include "media_fixture.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
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


VideoFixture matroska_fixture(std::string_view pcm16) {
  if (pcm16.size()%2 || pcm16.size()>30*16000*2) throw std::invalid_argument("invalid fixture PCM");
  Mux mux;
  struct Encoder {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    ~Encoder() { av_frame_free(&frame); avcodec_free_context(&codec); }
  } encoder;
  const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!codec) throw std::runtime_error("MJPEG fixture encoder unavailable");
  encoder.codec = avcodec_alloc_context3(codec);
  encoder.frame = av_frame_alloc();
  if (!encoder.codec || !encoder.frame) throw std::bad_alloc();
  encoder.codec->width = encoder.codec->height = 64;
  encoder.codec->pix_fmt = AV_PIX_FMT_YUVJ420P;
  encoder.codec->time_base = {1, 10};
  encoder.codec->thread_count = 1;
  require(avcodec_open2(encoder.codec, codec, nullptr));
  encoder.frame->width = encoder.frame->height = 64;
  encoder.frame->format = encoder.codec->pix_fmt;
  require(av_frame_get_buffer(encoder.frame, 32));
  require(avformat_alloc_output_context2(&mux.format, nullptr, "matroska", nullptr));
  auto* buffer = static_cast<unsigned char*>(av_malloc(4096));
  if (!buffer) throw std::bad_alloc();
#if LIBAVFORMAT_VERSION_MAJOR >= 61
  mux.io = avio_alloc_context(buffer, 4096, 1, &mux, nullptr, &Mux::write, nullptr);
#else
  mux.io = avio_alloc_context(buffer, 4096, 1, &mux, nullptr,
      [](void* opaque, uint8_t* data, int size) { return Mux::write(opaque, data, size); }, nullptr);
#endif
  if (!mux.io) { av_free(buffer); throw std::bad_alloc(); }
  mux.format->pb = mux.io;
  mux.format->flags |= AVFMT_FLAG_CUSTOM_IO;
  av_dict_set(&mux.format->metadata, "title", "Compressed live fixture", 0);
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
  video->time_base = encoder.codec->time_base;
  require(avcodec_parameters_from_context(video->codecpar, encoder.codec));
  AVDictionary* options = nullptr;
  av_dict_set(&options, "live", "1", 0);
  av_dict_set(&options, "cluster_time_limit", "200", 0);
  const auto header = avformat_write_header(mux.format, &options);
  av_dict_free(&options); require(header);
  mux.packet = av_packet_alloc();
  if (!mux.packet) throw std::bad_alloc();
  VideoFixture result;
  auto drain = [&] {
    while (true) {
      const int status = avcodec_receive_packet(encoder.codec, mux.packet);
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
      require(status);
      result.video_packets.emplace_back(reinterpret_cast<const char*>(mux.packet->data), mux.packet->size);
      mux.packet->stream_index = video->index;
      mux.packet->duration = 1;
      av_packet_rescale_ts(mux.packet, encoder.codec->time_base, video->time_base);
      require(av_interleaved_write_frame(mux.format, mux.packet));
    }
  };
  const size_t audio_bytes = pcm16.empty() ? 96000 : pcm16.size();
  for (size_t index = 0; index < (audio_bytes + 3199)/3200; ++index) {
    const auto count = std::min(size_t(3200), audio_bytes - index*3200);
    require(av_new_packet(mux.packet, static_cast<int>(count)));
    if (pcm16.empty()) std::fill_n(mux.packet->data, count, uint8_t{0});
    else std::memcpy(mux.packet->data, pcm16.data()+index*3200, count);
    mux.packet->stream_index = audio->index;
    mux.packet->pts = mux.packet->dts = av_rescale_q(index * 1600, {1, 16000}, audio->time_base);
    mux.packet->duration = av_rescale_q(count/2, {1, 16000}, audio->time_base);
    mux.packet->flags = AV_PKT_FLAG_KEY;
    require(av_interleaved_write_frame(mux.format, mux.packet));
    require(av_frame_make_writable(encoder.frame));
    for (int plane = 0; plane < 3; ++plane)
      for (int row = 0; row < (plane ? 32 : 64); ++row)
        std::fill_n(encoder.frame->data[plane] + row * encoder.frame->linesize[plane],
                    plane ? 32 : 64, static_cast<uint8_t>(plane ? 128 : 64 + index));
    encoder.frame->pts = index;
    require(avcodec_send_frame(encoder.codec, encoder.frame)); drain();
  }
  require(avcodec_send_frame(encoder.codec, nullptr)); drain();
  require(av_write_trailer(mux.format)); avio_flush(mux.io);
  if (mux.failure) std::rethrow_exception(mux.failure);
  result.bytes = std::move(mux.bytes);
  return result;
}
