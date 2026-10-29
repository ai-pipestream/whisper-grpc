#pragma once
#include "speech.h"
namespace media {
class ParakeetModel final : public SpeechBackend {
 public:
  ParakeetModel(std::span<const char> model, std::string model_id, int threads);
  ~ParakeetModel() override;
  void validate_options(const pb::SpeechOptions&) const override;
  std::vector<pb::TranscriptSegment> infer(const SpeechWindow&, uint32_t,
      const pb::SpeechOptions&, const std::function<bool()>&) override;
  const std::string& model_hash() const override;
  const std::string& model_id() const override;
  int threads() const override;
  std::optional<bool> multilingual_capability() const override { return std::nullopt; }
  bool supports_translation() const override { return false; }
  bool supports_language_hint() const override { return false; }
  bool reports_language() const override { return false; }
  std::optional<uint32_t> minimum_encoder_context_ms() const override { return 4000; }
  std::optional<pb::EncoderContext> encoder_context(size_t samples) const override;
  pb::SpeechBackendKind backend_kind() const override { return pb::SPEECH_BACKEND_KIND_PARAKEET; }
  std::string engine_version() const override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
