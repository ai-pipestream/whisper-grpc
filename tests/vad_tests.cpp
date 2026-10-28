#include "vad.h"
#include "windowing.h"
#include "decoder.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("VAD test asset missing");
  return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("usage: vad_tests SILERO_MODEL JFK_WAV");
    const auto model = read(argv[1]);
    const auto bytes = read(argv[2]);
    std::vector<float> speech;
    size_t offset = 0;
    media::pb::DecodeOptions decode; decode.set_emit_audio(true);
    media::decode_media([&](uint8_t* out, int cap) {
      const auto n = std::min(bytes.size() - offset, static_cast<size_t>(cap));
      std::memcpy(out, bytes.data() + offset, n); offset += n; return static_cast<int>(n);
    }, [&](media::pb::StreamEvent e) {
      if (!e.has_audio()) return;
      const auto& pcm = e.audio().pcm_f32le();
      for (size_t i = 0; i < pcm.size(); i += 4) {
        uint32_t bits = 0;
        for (int b = 0; b < 4; ++b) bits |= static_cast<uint32_t>(static_cast<unsigned char>(pcm[i+b])) << (8*b);
        speech.push_back(std::bit_cast<float>(bits));
      }
    }, [] { return false; }, decode);
    std::vector<float> audio(16000, 0.0f);
    audio.insert(audio.end(), speech.begin(), speech.end());
    audio.resize(audio.size() + 6 * 16000 + 17, 0.0f);
    std::vector<float> reference_probs;
    std::vector<media::SpeechWindow> reference_windows;
    for (size_t chunk : {size_t(1), size_t(137), size_t(4096)}) {
      std::vector<float> probs;
      std::vector<media::SpeechWindow> windows;
      media::Windowing windowing({48000, 80000, 8000, 4800, 1600, true, false},
                                 [&](auto w) { windows.push_back(std::move(w)); });
      size_t received = 0, speech_samples = 0;
      media::VoiceActivity vad(model, [&](std::span<const float> pcm, float probability) {
        check(pcm.size() <= 512 && received + pcm.size() <= audio.size(), "VAD padding escaped into source timeline");
        check(std::equal(pcm.begin(), pcm.end(), audio.begin() + received), "VAD changed source samples");
        received += pcm.size(); probs.push_back(probability);
        if (probability >= 0.5f) speech_samples += pcm.size();
        windowing.append(pcm, probability >= 0.5f);
      });
      for (size_t i = 0; i < audio.size(); i += chunk)
        vad.append(std::span(audio).subspan(i, std::min(chunk, audio.size() - i)));
      vad.finish(); windowing.finish();
      check(received == audio.size() && speech_samples > speech.size() / 4, "real speech not detected");
      check(std::any_of(windows.begin(), windows.end(), [](const auto& w) { return w.boundary == media::WindowBoundary::Pause; }),
            "real VAD selected no pause cuts");
      uint64_t through = 0;
      for (const auto& w : windows) {
        check(w.first_new_sample == through && w.samples.size() <= 80000, "unbounded or incomplete smart windows");
        check(std::equal(w.samples.begin(), w.samples.end(), audio.begin() + w.first_sample), "smart window changed audio");
        through = w.first_sample + w.samples.size();
      }
      check(through == audio.size(), "smart window tail missing");
      if (reference_probs.empty()) { reference_probs = probs; reference_windows = windows; }
      else {
        check(reference_probs == probs && windows.size() == reference_windows.size(), "VAD depends on transport framing");
        for (size_t i = 0; i < windows.size(); ++i)
          check(windows[i].first_sample == reference_windows[i].first_sample && windows[i].samples == reference_windows[i].samples &&
                windows[i].boundary == reference_windows[i].boundary, "smart boundaries depend on transport framing");
      }
    }
    std::vector<float> silence(1001, 0.0f);
    size_t count = 0;
    media::VoiceActivity silent(model, [&](auto pcm, float p) { count += pcm.size(); check(p < 0.5f, "silence classified as speech"); });
    silent.append(silence); silent.discontinuity(); silent.append(silence); silent.finish();
    check(count == 2002, "discontinuity lost or padded samples");
    bool rejected = false;
    try { silent.append(silence); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "VAD accepted samples after finish");
    rejected = false;
    try { media::VoiceActivity wrong("wrong", [](auto, auto) {}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "unqualified VAD model accepted");
    size_t quiet_speech = 0;
    media::VoiceActivity quiet(model, [&](auto pcm, float p) { if (p >= 0.5f) quiet_speech += pcm.size(); });
    for (auto& sample : speech) sample *= 0.1f;
    quiet.append(speech); quiet.finish();
    check(quiet_speech > speech.size() / 4, "attenuated reference speech not detected");
    media::VoiceActivity invalid(model, [](auto, auto) {});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    rejected = false;
    try { invalid.append(std::span(&nan, 1)); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "VAD accepted nonfinite audio");
    std::cout << "Real VAD speech, silence, smart boundaries, framing invariance and lifecycle tests passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
