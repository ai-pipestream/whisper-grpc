#pragma once
#include "speech.h"
#include <atomic>
namespace media {
class SpeechWorker final : public pb::SpeechWorkerService::Service {
 public:
  SpeechWorker(SpeechModel& model, std::string machine_id = {});
  grpc::Status Transcribe(grpc::ServerContext*,
      grpc::ServerReaderWriter<pb::TranscribeResponse, pb::TranscribeRequest>*) override;
 private:
  SpeechModel& model_;
  pb::Producer producer_;
  std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
};
}  // namespace media
