#include "speech.h"
#include "decoder.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("test asset missing");
  return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: speech_tests MODEL JFK_WAV");
    const auto weights = read(argv[1]);
    media::SpeechModel model(weights, "test-tiny.en", 4);
    check(model.model_hash().size() == 32, "missing model fingerprint");
    const auto bytes = read(argv[2]);
    size_t offset = 0;
    media::SpeechWindow window{1, 16000, 16000, media::WindowBoundary::End, {}};
    media::pb::DecodeOptions decode;
    decode.set_emit_audio(true);
    media::decode_media([&](uint8_t* out, int cap) {
      const auto n = std::min(bytes.size() - offset, static_cast<size_t>(cap));
      std::memcpy(out, bytes.data() + offset, n); offset += n; return static_cast<int>(n);
    }, [&](media::pb::StreamEvent event) {
      if (!event.has_audio()) return;
      const auto& pcm = event.audio().pcm_f32le();
      for (size_t i = 0; i < pcm.size(); i += 4) {
        uint32_t bits = 0;
        for (int b = 0; b < 4; ++b) bits |= static_cast<uint32_t>(static_cast<unsigned char>(pcm[i+b])) << (8*b);
        window.samples.push_back(std::bit_cast<float>(bits));
      }
    }, [] { return false; }, decode);
    media::pb::SpeechOptions options;
    options.set_model_id("test-tiny.en"); options.set_language("en"); options.set_word_timestamps(true);
    const auto segments = model.infer(window, 7, options, [] { return false; });
    std::string transcript;
    for (const auto& s : segments) {
      check(s.track_id() == 7 && !s.final() && s.revision() == 1, "incorrect segment identity");
      check(s.start_us() >= 1000000 && s.end_us() >= s.start_us(), "incorrect absolute timestamp");
      check(s.tokens_size() > 0 && s.source_language() == "en", "missing native token metadata");
      transcript += s.text();
    }
    std::cout << transcript << '\n';
    std::transform(transcript.begin(), transcript.end(), transcript.begin(), [](unsigned char c) { return std::tolower(c); });
    check(transcript.find("ask not") != std::string::npos && transcript.find("country") != std::string::npos,
          "reference speech not recognized");
    bool cancelled = false;
    try { model.infer(window, 7, options, [] { return true; }); }
    catch (const media::Failure& f) { cancelled = f.code == grpc::StatusCode::CANCELLED; }
    check(cancelled, "pre-cancelled inference ran");
    options.set_model_id("unregistered");
    bool rejected = false;
    try { model.infer(window, 7, options, [] { return false; }); }
    catch (const media::Failure& f) { rejected = f.code == grpc::StatusCode::INVALID_ARGUMENT; }
    check(rejected, "model mismatch ignored");
    options.set_model_id("test-tiny.en");
    options.set_language("fr");
    rejected = false;
    try { model.infer(window, 7, options, [] { return false; }); }
    catch (const media::Failure& f) { rejected = f.code == grpc::StatusCode::INVALID_ARGUMENT; }
    check(rejected, "English-only model accepted another language");
    std::cout << "Native speech recognition, cancellation and model identity tests passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
