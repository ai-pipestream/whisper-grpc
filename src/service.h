#pragma once
#include "registry.h"
#include "processing.h"

namespace media {
class Service final : public pb::MediaService::Service {
 public:
  explicit Service(Registry& registry, const ProcessingFactory* processing = nullptr)
      : registry_(registry), processing_(processing) {}
  grpc::Status Ingest(grpc::ServerContext*, grpc::ServerReaderWriter<pb::IngestResponse, pb::IngestRequest>*) override;
  grpc::Status Subscribe(grpc::ServerContext*, const pb::SubscribeRequest*, grpc::ServerWriter<pb::SubscribeResponse>*) override;
 private:
  Registry& registry_;
  const ProcessingFactory* processing_;
};
}  // namespace media
