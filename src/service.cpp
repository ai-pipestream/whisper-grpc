#include "service.h"
#include "decoder.h"

#include <openssl/evp.h>

#include <array>
#include <cstring>
#include <limits>

namespace media {
namespace {
class Digest {
 public:
  Digest() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("digest initialization failed");
  }
  void append(const std::string& data) {
    if (EVP_DigestUpdate(context_.get(), data.data(), data.size()) != 1)
      throw std::runtime_error("digest update failed");
  }
  std::string finish() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_.get(), output.data(), &size) != 1)
      throw std::runtime_error("digest finalization failed");
    return {reinterpret_cast<const char*>(output.data()), size};
  }
 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};
}  // namespace

grpc::Status Service::Ingest(grpc::ServerContext* context, grpc::ServerReaderWriter<pb::IngestResponse, pb::IngestRequest>* wire) {
  std::string id;
  uint64_t count = 0, bytes = 0;
  grpc::Status status;
  bool explicit_finish = false;
  pb::FinishStream finish;
  auto send = [&](const pb::StreamEvent& event) {
    pb::IngestResponse response;
    *response.mutable_event() = event;
    if (!wire->Write(response)) throw Failure(grpc::StatusCode::CANCELLED, "ingress response disconnected");
  };
  try {
    Digest digest;
    try {
      pb::IngestRequest request;
      if (!wire->Read(&request) || !request.has_open())
        throw Failure(grpc::StatusCode::INVALID_ARGUMENT, "first message must open a stream");
      const auto opened = registry_.open(request.open());
      const auto options = request.open().decode();
      const bool decoding = request.open().has_decode();
      id = opened.cursor().stream_id();
      send(opened);
      std::string buffered;
      size_t offset = 0;
      auto next_chunk = [&]() -> bool {
        if (explicit_finish) return false;
        if (!wire->Read(&request)) throw Failure(grpc::StatusCode::CANCELLED, "input ended without FinishStream");
        if (request.has_finish()) {
          finish = request.finish();
          explicit_finish = true;
          return false;
        }
        if (!request.has_chunk()) throw Failure(grpc::StatusCode::INVALID_ARGUMENT, "expected a source chunk or finish");
        auto chunk = request.chunk();
        if (chunk.data().empty()) throw Failure(grpc::StatusCode::INVALID_ARGUMENT, "empty source chunk");
        if (chunk.data().size() > registry_.limits().max_chunk_bytes)
          throw Failure(grpc::StatusCode::RESOURCE_EXHAUSTED, "source chunk exceeds negotiated limit");
        if (count == std::numeric_limits<uint64_t>::max() || chunk.chunk_number() != count + 1 || chunk.byte_offset() != bytes)
          throw Failure(grpc::StatusCode::INVALID_ARGUMENT, "noncontiguous source chunk number or byte offset");
        if (chunk.data().size() > std::numeric_limits<uint64_t>::max() - bytes)
          throw Failure(grpc::StatusCode::RESOURCE_EXHAUSTED, "source byte counter overflow");
        const auto hash = sha256(chunk.data());
        if (!chunk.sha256().empty() && chunk.sha256() != hash)
          throw Failure(grpc::StatusCode::INVALID_ARGUMENT, "source chunk checksum mismatch");
        chunk.set_sha256(hash);
        digest.append(chunk.data());
        ++count;
        bytes += chunk.data().size();
        send(registry_.source(id, chunk));
        buffered = std::move(*chunk.mutable_data());
        offset = 0;
        return true;
      };
      if (decoding) {
        const auto library_version = decoder_version();
        decode_media([&](uint8_t* output, int capacity) -> int {
          if (offset == buffered.size() && !next_chunk()) return 0;
          const auto size = std::min(buffered.size() - offset, static_cast<size_t>(capacity));
          std::memcpy(output, buffered.data() + offset, size);
          offset += size;
          return static_cast<int>(size);
        }, [&](pb::StreamEvent event) {
          send(registry_.derived(id, std::move(event), library_version));
        }, [&] { return context->IsCancelled(); }, options);
      }
      // Some finite formats stop at their declared data length. Preserve any
      // remaining original bytes and still require the explicit input trailer.
      while (next_chunk()) {}
      if (!explicit_finish) throw Failure(grpc::StatusCode::CANCELLED, "input ended without FinishStream");
      if (finish.chunk_count() != count || finish.byte_count() != bytes)
        throw Failure(grpc::StatusCode::INVALID_ARGUMENT, "finish accounting mismatch");
    } catch (const Failure& error) {
      status = {error.code, error.what()};
    } catch (const std::exception&) {
      status = {grpc::StatusCode::INTERNAL, "ingress processing failed"};
    }
    if (id.empty()) return status;
    const auto hash = digest.finish();
    if (status.ok() && !finish.sha256().empty() && finish.sha256() != hash)
      status = {grpc::StatusCode::INVALID_ARGUMENT, "complete source checksum mismatch"};
    if (context->IsCancelled()) status = {grpc::StatusCode::CANCELLED, "ingress cancelled"};
    pb::StreamClosed closed;
    closed.set_status_code(status.error_code());
    closed.set_message(status.error_message());
    closed.set_source_chunk_count(count);
    closed.set_source_byte_count(bytes);
    closed.set_source_sha256(hash);
    const auto event = registry_.close(id, closed);
    // Publish terminal state for observers even if the originating client left.
    pb::IngestResponse response;
    *response.mutable_event() = event;
    if (!wire->Write(response) && status.ok())
      return {grpc::StatusCode::CANCELLED, "terminal response disconnected"};
    return status;
  } catch (const std::exception&) {
    return {grpc::StatusCode::INTERNAL, "ingress finalization failed"};
  }
}

grpc::Status Service::Subscribe(grpc::ServerContext* context, const pb::SubscribeRequest* request, grpc::ServerWriter<pb::SubscribeResponse>* wire) {
  try {
    auto subscription = registry_.subscribe(*request);
    pb::SubscribeResponse response;
    while (!context->IsCancelled()) {
      if (subscription->next(response, std::chrono::milliseconds(50))) {
        if (!wire->Write(response)) break;
      } else if (subscription->stopped()) {
        return subscription->status();
      }
    }
    subscription->cancel();
    return {grpc::StatusCode::CANCELLED, "subscription disconnected"};
  } catch (const Failure& error) {
    return {error.code, error.what()};
  } catch (const std::exception&) {
    return {grpc::StatusCode::INTERNAL, "subscription failed"};
  }
}
}  // namespace media
