#include "registry.h"

#include <grpc/grpc.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace media {
namespace {
size_t charge(const google::protobuf::MessageLite& message) {
  // Include a fixed allowance for queue nodes and protobuf object bookkeeping.
  return message.ByteSizeLong() + 512;
}
void require(bool condition, grpc::StatusCode code, const char* message) {
  if (!condition) throw Failure(code, message);
}
}  // namespace

std::string uuid() {
  std::array<unsigned char, 16> data{};
  if (RAND_bytes(data.data(), data.size()) != 1) throw std::runtime_error("UUID entropy failed");
  data[6] = (data[6] & 0x0f) | 0x40;
  data[8] = (data[8] & 0x3f) | 0x80;
  const char* hex = "0123456789abcdef";
  std::string result;
  for (size_t i = 0; i < data.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
    result += hex[data[i] >> 4];
    result += hex[data[i] & 15];
  }
  return result;
}

bool valid_uuid(const std::string& value) {
  if (value.size() != 36) return false;
  bool nonzero = false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else {
      const char c = value[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
      nonzero |= c != '0';
    }
  }
  return nonzero;
}

std::string sha256(const std::string& bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 failed");
  return {reinterpret_cast<const char*>(digest.data()), length};
}

Subscription::Subscription(const pb::SubscribeRequest& request, size_t limit)
    : ids_(request.stream_ids().begin(), request.stream_ids().end()),
      kinds_(request.payload_kinds().begin(), request.payload_kinds().end()), limit_(limit) {}

bool Subscription::accepts(const std::string& id) const { return ids_.empty() || ids_.contains(id); }
bool Subscription::accepts(const pb::StreamEvent& event) const {
  if (!accepts(event.cursor().stream_id())) return false;
  if (kinds_.empty() || event.has_opened() || event.has_closed()) return true;
  return (event.has_source() && kinds_.contains(pb::PAYLOAD_KIND_SOURCE)) ||
         (event.has_audio() && kinds_.contains(pb::PAYLOAD_KIND_AUDIO)) ||
         (event.has_transcript() && kinds_.contains(pb::PAYLOAD_KIND_TRANSCRIPT));
}
bool Subscription::push(pb::SubscribeResponse response) {
  std::lock_guard lock(mutex_);
  if (stopped_) return false;
  const auto size = charge(response);
  if (size > limit_ || bytes_ > limit_ - size) {
    stopped_ = true;
    status_ = {grpc::StatusCode::RESOURCE_EXHAUSTED, "subscriber queue exhausted; resume using last delivered cursor"};
    pending_.clear();
    bytes_ = 0;
    changed_.notify_all();
    return false;
  }
  bytes_ += size;
  pending_.push_back(std::move(response));
  changed_.notify_one();
  return true;
}
bool Subscription::next(pb::SubscribeResponse& response, std::chrono::milliseconds wait) {
  std::unique_lock lock(mutex_);
  changed_.wait_for(lock, wait, [&] { return stopped_ || !pending_.empty(); });
  if (pending_.empty()) return false;
  bytes_ -= charge(pending_.front());
  response = std::move(pending_.front());
  pending_.pop_front();
  return true;
}
void Subscription::cancel() {
  std::lock_guard lock(mutex_);
  stopped_ = true;
  pending_.clear();
  bytes_ = 0;
  changed_.notify_all();
}
bool Subscription::stopped() const { std::lock_guard lock(mutex_); return stopped_; }
grpc::Status Subscription::status() const { std::lock_guard lock(mutex_); return status_; }

Registry::Registry(Limits limits, std::string machine_id) : limits_(limits), epoch_(uuid()) {
  if (!limits.max_chunk_bytes || !limits.retention_bytes || !limits.subscriber_bytes ||
      !limits.max_streams || !limits.max_subscribers)
    throw std::invalid_argument("registry limits must be positive");
  if (!machine_id.empty() && !valid_uuid(machine_id)) throw std::invalid_argument("invalid machine UUID");
  producer_.set_machine_id(machine_id.empty() ? uuid() : machine_id);
  producer_.set_service("whisper-media-server");
  producer_.set_app_version(MEDIA_APP_VERSION);
  producer_.set_build_id(MEDIA_BUILD_ID);
  producer_.set_execution_backend("cpu");
  producer_.set_library_version(std::string("grpc/") + grpc_version_string());
  producer_.set_configuration_sha256(sha256(
      "max_chunk_bytes=" + std::to_string(limits.max_chunk_bytes) +
      ";retention_bytes=" + std::to_string(limits.retention_bytes) +
      ";subscriber_bytes=" + std::to_string(limits.subscriber_bytes) +
      ";max_streams=" + std::to_string(limits.max_streams) +
      ";max_subscribers=" + std::to_string(limits.max_subscribers)));
}

pb::StreamEvent Registry::publish(const std::string& id, Stream& stream, pb::StreamEvent event) {
  auto* cursor = event.mutable_cursor();
  cursor->set_server_epoch(epoch_);
  cursor->set_stream_id(id);
  cursor->set_event_sequence(++stream.sequence);
  *event.mutable_producer() = producer_;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
  event.mutable_emitted_at()->set_seconds(seconds.count());
  event.mutable_emitted_at()->set_nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds).count());
  *stream.snapshot.mutable_through() = *cursor;
  const auto size = charge(event);
  while (!stream.history.empty() && (size > limits_.retention_bytes || stream.history_bytes > limits_.retention_bytes - size)) {
    stream.history_bytes -= charge(stream.history.front());
    stream.history.pop_front();
  }
  if (size <= limits_.retention_bytes) {
    stream.history.push_back(event);
    stream.history_bytes += size;
  }
  stream.snapshot.set_first_retained_sequence(stream.history.empty() ? stream.sequence + 1 : stream.history.front().cursor().event_sequence());
  // Keep overflowed listeners counted until their RPC releases them. A listener
  // may still be blocked in a transport write, and must not free capacity early.
  std::erase_if(subscribers_, [](const auto& weak) { return weak.expired(); });
  for (const auto& weak : subscribers_) {
    if (auto s = weak.lock(); s && s->accepts(event)) {
      pb::SubscribeResponse response;
      *response.mutable_event() = event;
      s->push(std::move(response));
    }
  }
  return event;
}

pb::StreamEvent Registry::open(const pb::OpenStream& request) {
  require(request.source_case() != pb::OpenStream::SOURCE_NOT_SET, grpc::StatusCode::INVALID_ARGUMENT, "source is required");
  require(request.has_direct(), grpc::StatusCode::UNIMPLEMENTED, "only direct ingress is implemented in this build");
  require(request.speech().empty(), grpc::StatusCode::UNIMPLEMENTED, "speech worker is not connected in this build");
  require(request.direct().name().size() <= 1024 && request.direct().media_type().size() <= 256,
          grpc::StatusCode::INVALID_ARGUMENT, "source description too long");
  const auto id = request.stream_id().empty() ? uuid() : request.stream_id();
  require(valid_uuid(id), grpc::StatusCode::INVALID_ARGUMENT, "stream_id must be a non-nil lowercase UUID");
  std::lock_guard lock(mutex_);
  require(!streams_.contains(id), grpc::StatusCode::ALREADY_EXISTS, "stream UUID already used in this epoch");
  require(streams_.size() < limits_.max_streams, grpc::StatusCode::RESOURCE_EXHAUSTED, "registry stream limit reached");
  auto& stream = streams_[id];
  pb::StreamEvent event;
  auto* opened = event.mutable_opened();
  *opened->mutable_source() = request.direct();
  opened->set_max_chunk_bytes(limits_.max_chunk_bytes);
  opened->set_retention_bytes(limits_.retention_bytes);
  *stream.snapshot.mutable_opened() = *opened;
  *stream.snapshot.add_producers() = producer_;
  return publish(id, stream, std::move(event));
}

pb::StreamEvent Registry::source(const std::string& id, const pb::SourceChunk& chunk) {
  std::lock_guard lock(mutex_);
  auto it = streams_.find(id);
  require(it != streams_.end(), grpc::StatusCode::NOT_FOUND, "unknown stream");
  require(!it->second.closed, grpc::StatusCode::FAILED_PRECONDITION, "stream is closed");
  pb::StreamEvent event;
  *event.mutable_source() = chunk;
  return publish(id, it->second, std::move(event));
}

pb::StreamEvent Registry::close(const std::string& id, const pb::StreamClosed& closed) {
  std::lock_guard lock(mutex_);
  auto it = streams_.find(id);
  require(it != streams_.end(), grpc::StatusCode::NOT_FOUND, "unknown stream");
  require(!it->second.closed, grpc::StatusCode::FAILED_PRECONDITION, "stream is already closed");
  it->second.closed = true;
  *it->second.snapshot.mutable_closed() = closed;
  pb::StreamEvent event;
  *event.mutable_closed() = closed;
  return publish(id, it->second, std::move(event));
}

std::shared_ptr<Subscription> Registry::subscribe(const pb::SubscribeRequest& request) {
  require(request.stream_ids_size() <= static_cast<int>(limits_.max_streams), grpc::StatusCode::RESOURCE_EXHAUSTED, "too many selected streams");
  require(request.hydration().after_size() <= static_cast<int>(limits_.max_streams), grpc::StatusCode::RESOURCE_EXHAUSTED, "too many replay cursors");
  for (const auto& id : request.stream_ids()) require(valid_uuid(id), grpc::StatusCode::INVALID_ARGUMENT, "invalid stream UUID");
  for (int kind : request.payload_kinds()) require(kind >= pb::PAYLOAD_KIND_SOURCE && kind <= pb::PAYLOAD_KIND_TRANSCRIPT, grpc::StatusCode::INVALID_ARGUMENT, "invalid payload kind");
  const auto& hydration = request.hydration();
  require(hydration.after().empty() || hydration.retained_events(), grpc::StatusCode::INVALID_ARGUMENT, "replay cursors require retained_events");
  auto sub = std::make_shared<Subscription>(request, limits_.subscriber_bytes);
  std::lock_guard lock(mutex_);
  std::erase_if(subscribers_, [](const auto& weak) { return weak.expired(); });
  require(subscribers_.size() < limits_.max_subscribers, grpc::StatusCode::RESOURCE_EXHAUSTED, "subscriber limit reached");
  std::map<std::string, uint64_t> after;
  for (const auto& cursor : hydration.after()) {
    require(cursor.server_epoch() == epoch_, grpc::StatusCode::OUT_OF_RANGE, "replay epoch unavailable");
    require(sub->accepts(cursor.stream_id()), grpc::StatusCode::INVALID_ARGUMENT, "cursor outside stream selection");
    auto it = streams_.find(cursor.stream_id());
    require(it != streams_.end(), grpc::StatusCode::OUT_OF_RANGE, "replay stream unavailable");
    require(cursor.event_sequence() <= it->second.sequence, grpc::StatusCode::OUT_OF_RANGE, "cursor ahead of stream");
    require(cursor.event_sequence() >= it->second.snapshot.first_retained_sequence() - 1, grpc::StatusCode::OUT_OF_RANGE, "requested history evicted");
    require(after.emplace(cursor.stream_id(), cursor.event_sequence()).second, grpc::StatusCode::INVALID_ARGUMENT, "duplicate replay cursor");
  }
  auto enqueue = [&](pb::SubscribeResponse response) {
    require(sub->push(std::move(response)), grpc::StatusCode::RESOURCE_EXHAUSTED, "hydration exceeds subscriber budget; narrow selection or resume later");
  };
  if (hydration.metadata() || hydration.retained_events()) {
    const auto snapshot_id = uuid();
    pb::SubscribeResponse begin;
    begin.mutable_snapshot_begin()->set_snapshot_id(snapshot_id);
    for (const auto& [id, stream] : streams_) if (sub->accepts(id))
      *begin.mutable_snapshot_begin()->add_through() = stream.snapshot.through();
    enqueue(std::move(begin));
    for (const auto& [id, stream] : streams_) {
      if (!sub->accepts(id)) continue;
      if (hydration.metadata()) {
        pb::SubscribeResponse response;
        *response.mutable_snapshot() = stream.snapshot;
        enqueue(std::move(response));
      }
      if (hydration.retained_events()) for (const auto& event : stream.history) {
        auto found = after.find(id);
        if ((found == after.end() || event.cursor().event_sequence() > found->second) && sub->accepts(event)) {
          pb::SubscribeResponse response;
          *response.mutable_event() = event;
          enqueue(std::move(response));
        }
      }
    }
    pb::SubscribeResponse end;
    end.mutable_snapshot_end()->set_snapshot_id(snapshot_id);
    enqueue(std::move(end));
  }
  subscribers_.push_back(sub);
  return sub;
}
}  // namespace media
