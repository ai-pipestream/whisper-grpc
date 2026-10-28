#pragma once
#include "registry.h"
#include <functional>
namespace media {
using EmitProcessed = std::function<void(pb::StreamEvent)>;
class Processing {
 public:
  virtual ~Processing() = default;
  virtual void audio(const pb::AudioChunk&) = 0;
  virtual void finish() = 0;
};
class ProcessingFactory {
 public:
  virtual ~ProcessingFactory() = default;
  virtual void resolve(pb::OpenStream&) const = 0;
  virtual std::unique_ptr<Processing> create(const std::string& id, const pb::OpenStream&,
                                            grpc::ServerContext&, EmitProcessed) const = 0;
};
}  // namespace media
