#pragma once
#include "registry.h"
#include "windowing.h"
#include <functional>
#include <memory>
#include <span>

namespace media {
// A native CPU model instance. Model bytes are supplied by operator-controlled
// startup configuration, never by an inference request. Calls are serialized.
class SpeechModel {
 public:
  SpeechModel(std::span<const char> model, std::string model_id, int threads);
  ~SpeechModel();
  SpeechModel(const SpeechModel&) = delete;
  SpeechModel& operator=(const SpeechModel&) = delete;
  std::vector<pb::TranscriptSegment> infer(const SpeechWindow& window, uint32_t track,
                                         const pb::SpeechOptions& options,
                                         const std::function<bool()>& cancelled);
  const std::string& model_hash() const;
  const std::string& model_id() const;
  static std::string library_version();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace media
