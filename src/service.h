#pragma once
#include "registry.h"

namespace media {
class Service final : public pb::MediaService::Service {
 public:
  explicit Service(Registry& registry) : registry_(registry) {}
  grpc::Status Ingest(grpc::ServerContext*, grpc::ServerReaderWriter<pb::IngestResponse, pb::IngestRequest>*) override;
  grpc::Status Subscribe(grpc::ServerContext*, const pb::SubscribeRequest*, grpc::ServerWriter<pb::SubscribeResponse>*) override;
 private:
  Registry& registry_;
};
}  // namespace media
