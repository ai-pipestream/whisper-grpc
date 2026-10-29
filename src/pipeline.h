#pragma once
#include "processing.h"
namespace media {
class SpeechPipelineFactory final : public ProcessingFactory {
 public:
  static constexpr uint32_t minimum_advance_ms = 100;
  std::optional<uint32_t> minimum_window_advance_ms() const override { return minimum_advance_ms; }
  SpeechPipelineFactory(std::string target, std::string vad_weights = {}, std::string machine_id = {});
  void resolve(pb::OpenStream&) const override;
  std::unique_ptr<Processing> create(const std::string&, const pb::OpenStream&, grpc::ServerContext&, EmitProcessed) const override;
 private:
  std::string target_, vad_weights_, machine_id_;
};
}  // namespace media
