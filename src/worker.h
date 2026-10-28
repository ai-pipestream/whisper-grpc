#pragma once
#include "speech.h"
#include <atomic>
#include <chrono>
namespace media {
class SpeechWorker final : public pb::SpeechWorkerService::Service {
 public:
  SpeechWorker(SpeechBackend& model, std::string machine_id = {},
               std::chrono::milliseconds input_idle_timeout = std::chrono::seconds(120));
  grpc::Status GetModelInfo(grpc::ServerContext*, const pb::GetModelInfoRequest*, pb::GetModelInfoResponse*) override;
  grpc::Status Transcribe(grpc::ServerContext*,
      grpc::ServerReaderWriter<pb::TranscribeResponse, pb::TranscribeRequest>*) override;
 private:
  static constexpr unsigned max_streams = 32;
  static constexpr unsigned max_window_samples = 480000;
  SpeechBackend& model_;
  std::chrono::milliseconds input_idle_timeout_;
  pb::Producer producer_;
  std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
  std::atomic<unsigned> streams_{0};
};
}  // namespace media
