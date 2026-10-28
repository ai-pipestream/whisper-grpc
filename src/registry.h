#pragma once

#include "ai/pipestream/media/v1/media.grpc.pb.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace media {
namespace pb = ai::pipestream::media::v1;

struct Failure : std::runtime_error {
  grpc::StatusCode code;
  Failure(grpc::StatusCode code, const std::string& message)
      : std::runtime_error(message), code(code) {}
};

struct Limits {
  size_t max_chunk_bytes = 256 * 1024;
  size_t retention_bytes = 1024 * 1024;
  size_t subscriber_bytes = 4 * 1024 * 1024;
  size_t max_streams = 64;
  size_t max_subscribers = 64;
};

std::string uuid();
bool valid_uuid(const std::string& value);
std::string sha256(const std::string& bytes);

// A bounded mailbox. Overflow terminates only this listener, never the producer.
class Subscription {
 public:
  Subscription(const pb::SubscribeRequest& request, size_t limit);
  bool accepts(const std::string& id) const;
  bool accepts(const pb::StreamEvent& event) const;
  bool push(pb::SubscribeResponse response);
  bool next(pb::SubscribeResponse& response, std::chrono::milliseconds wait);
  void cancel();
  bool stopped() const;
  grpc::Status status() const;

 private:
  const std::set<std::string> ids_;
  const std::set<int> kinds_;
  const size_t limit_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<pb::SubscribeResponse> pending_;
  size_t bytes_ = 0;
  bool stopped_ = false;
  grpc::Status status_;
};

// Registry lock serializes snapshot registration with every publication.
class Registry {
 public:
  explicit Registry(Limits limits = {}, std::string machine_id = {});
  pb::StreamEvent open(const pb::OpenStream& request, bool speech_enabled = false, bool http_enabled = false);
  pb::StreamEvent source(const std::string& id, const pb::SourceChunk& chunk);
  pb::StreamEvent derived(const std::string& id, pb::StreamEvent event, const std::string& library_version);
  pb::StreamEvent close(const std::string& id, const pb::StreamClosed& closed);
  std::shared_ptr<Subscription> subscribe(const pb::SubscribeRequest& request);
  const Limits& limits() const { return limits_; }
  const std::string& epoch() const { return epoch_; }
  const std::string& machine_id() const { return producer_.machine_id(); }
  const pb::Producer& producer() const { return producer_; }

 private:
  struct Stream {
    pb::Producer producer;
    pb::StreamSnapshot snapshot;
    std::deque<pb::StreamEvent> history;
    size_t history_bytes = 0;
    uint64_t sequence = 0;
    bool closed = false;
    std::string incarnation;
    std::chrono::steady_clock::time_point closed_at;
  };
  pb::StreamEvent publish(const std::string& id, Stream& stream, pb::StreamEvent event);
  Limits limits_;
  std::string epoch_;
  pb::Producer producer_;
  std::mutex mutex_;
  std::map<std::string, Stream> streams_;
  std::vector<std::weak_ptr<Subscription>> subscribers_;
};
}  // namespace media
