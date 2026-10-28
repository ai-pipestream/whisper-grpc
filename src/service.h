#pragma once
#include "registry.h"
#include "processing.h"
#include "http.h"

namespace media {
class Service final : public pb::MediaService::Service {
 public:
  explicit Service(Registry& registry, const ProcessingFactory* processing = nullptr, HttpPolicy http = {})
      : registry_(registry), processing_(processing), http_(std::move(http)) {}
  grpc::Status Ingest(grpc::ServerContext*, grpc::ServerReaderWriter<pb::IngestResponse, pb::IngestRequest>*) override;
  grpc::Status Subscribe(grpc::ServerContext*, const pb::SubscribeRequest*, grpc::ServerWriter<pb::SubscribeResponse>*) override;
  grpc::Status Pull(grpc::ServerContext*, const pb::PullRequest*, grpc::ServerWriter<pb::PullResponse>*) override;
 private:
  Registry& registry_;
  const ProcessingFactory* processing_;
  HttpPolicy http_;
  grpc::Status process(grpc::ServerContext*, const std::function<bool(pb::IngestRequest*)>&,
                       const std::function<bool(const pb::IngestResponse&)>&, bool http = false);
};
}  // namespace media
