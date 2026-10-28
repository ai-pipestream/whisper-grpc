#include "service.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace pb = media::pb;

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
void fails(grpc::StatusCode code, const std::function<void()>& call) {
  try { call(); } catch (const media::Failure& failure) {
    check(failure.code == code, "wrong failure status");
    return;
  }
  throw std::runtime_error("expected failure was accepted");
}
pb::OpenStream open_request(const std::string& id = {}) {
  pb::OpenStream result;
  result.set_stream_id(id);
  result.mutable_direct()->set_name("test-media");
  result.mutable_direct()->set_media_type("application/octet-stream");
  return result;
}
pb::SourceChunk chunk(uint64_t index, const std::string& data) {
  pb::SourceChunk result;
  result.set_chunk_number(index);
  result.set_byte_offset((index - 1) * data.size());
  result.set_data(data);
  return result;
}
pb::SubscribeResponse next(const std::shared_ptr<media::Subscription>& sub) {
  pb::SubscribeResponse response;
  check(sub->next(response, 1s), "missing subscription response");
  return response;
}

void uuid_selection_and_future_streams() {
  media::Registry registry;
  const auto wanted = media::uuid();
  check(media::valid_uuid(wanted), "generated UUID invalid");
  check(!media::valid_uuid("00000000-0000-0000-0000-000000000000"), "nil UUID accepted");
  pb::SubscribeRequest filter;
  filter.add_stream_ids(wanted);
  auto selected = registry.subscribe(filter);
  auto global = registry.subscribe({});
  const auto other = registry.open(open_request());
  check(next(global).event().cursor().stream_id() == other.cursor().stream_id(), "global missing new stream");
  pb::SubscribeResponse response;
  check(!selected->next(response, 0ms), "UUID filter leaked another stream");
  registry.open(open_request(wanted));
  check(next(selected).event().cursor().stream_id() == wanted, "future UUID not selected");
  check(next(global).event().cursor().stream_id() == wanted, "global missing second new stream");
  fails(grpc::StatusCode::ALREADY_EXISTS, [&] { registry.open(open_request(wanted)); });
  fails(grpc::StatusCode::INVALID_ARGUMENT, [&] { registry.open(open_request("not-a-uuid")); });
  filter.add_stream_ids("bad");
  fails(grpc::StatusCode::INVALID_ARGUMENT, [&] { registry.subscribe(filter); });
}

void snapshot_live_atomicity() {
  media::Limits limits;
  limits.retention_bytes = 8 * 1024 * 1024;
  limits.subscriber_bytes = 8 * 1024 * 1024;
  media::Registry registry(limits);
  const auto opened = registry.open(open_request());
  const auto id = opened.cursor().stream_id();
  for (int i = 1; i <= 20; ++i) registry.source(id, chunk(i, "x"));
  std::atomic<bool> start{false};
  std::jthread writer([&] {
    while (!start.load()) std::this_thread::yield();
    for (int i = 21; i <= 200; ++i) registry.source(id, chunk(i, "x"));
    registry.close(id, {});
  });
  pb::SubscribeRequest request;
  request.mutable_hydration()->set_metadata(true);
  request.mutable_hydration()->set_retained_events(true);
  start.store(true);
  auto sub = registry.subscribe(request);
  writer.join();
  const auto begin = next(sub);
  check(begin.has_snapshot_begin(), "snapshot did not begin");
  check(begin.snapshot_begin().through_size() == 1, "snapshot missing high water mark");
  const auto watermark = begin.snapshot_begin().through(0).event_sequence();
  const auto state = next(sub);
  check(state.has_snapshot(), "snapshot state missing");
  check(state.snapshot().through().event_sequence() == watermark, "state not at frozen boundary");
  uint64_t expected = 1;
  bool live = false;
  while (expected <= 202) {
    const auto response = next(sub);
    if (response.has_snapshot_end()) {
      check(!live, "duplicate snapshot end");
      check(response.snapshot_end().snapshot_id() == begin.snapshot_begin().snapshot_id(), "snapshot ID mismatch");
      check(expected == watermark + 1, "hydration missing events");
      live = true;
      continue;
    }
    check(response.has_event(), "unexpected payload");
    check(response.event().cursor().event_sequence() == expected++, "gap or duplicate across snapshot boundary");
    check(live ? response.event().cursor().event_sequence() > watermark : response.event().cursor().event_sequence() <= watermark,
          "event on wrong side of snapshot boundary");
  }
  // If the producer finished before registration, SnapshotEnd follows all events.
  if (!live) check(next(sub).has_snapshot_end(), "missing final snapshot boundary");
}

void replay_eviction_and_filtering() {
  media::Limits limits;
  limits.retention_bytes = 2500;
  media::Registry registry(limits);
  const auto opened = registry.open(open_request());
  const auto id = opened.cursor().stream_id();
  for (int i = 1; i <= 8; ++i) registry.source(id, chunk(i, std::string(500, 'a')));
  pb::SubscribeRequest request;
  request.mutable_hydration()->set_retained_events(true);
  auto* cursor = request.mutable_hydration()->add_after();
  *cursor = opened.cursor();
  fails(grpc::StatusCode::OUT_OF_RANGE, [&] { registry.subscribe(request); });
  cursor->set_event_sequence(9);
  auto sub = registry.subscribe(request);
  check(next(sub).has_snapshot_begin(), "resume missing boundary");
  check(next(sub).has_snapshot_end(), "resume replayed old events");
  registry.source(id, chunk(9, "b"));
  check(next(sub).event().cursor().event_sequence() == 10, "resume missing next event");
  cursor->set_server_epoch(media::uuid());
  fails(grpc::StatusCode::OUT_OF_RANGE, [&] { registry.subscribe(request); });
  pb::SubscribeRequest filtered;
  filtered.add_payload_kinds(pb::PAYLOAD_KIND_TRANSCRIPT);
  auto only_results = registry.subscribe(filtered);
  registry.source(id, chunk(10, "b"));
  pb::SubscribeResponse response;
  check(!only_results->next(response, 0ms), "payload filter leaked source bytes");
  registry.close(id, {});
  check(next(only_results).event().has_closed(), "filter hid lifecycle event");
  fails(grpc::StatusCode::FAILED_PRECONDITION, [&] { registry.source(id, chunk(11, "b")); });
}

void subscriber_backpressure_isolation() {
  media::Limits limits;
  limits.subscriber_bytes = 4000;
  media::Registry registry(limits);
  const auto id = registry.open(open_request()).cursor().stream_id();
  auto slow = registry.subscribe({});
  auto fast = registry.subscribe({});
  for (int i = 1; i <= 10; ++i) {
    registry.source(id, chunk(i, std::string(500, 'x')));
    check(next(fast).event().source().chunk_number() == static_cast<uint64_t>(i), "slow subscriber blocked fast one");
  }
  check(slow->stopped(), "slow subscriber grew without bound");
  check(slow->status().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED, "slow subscriber failed silently");
}

void bounded_registry_and_capabilities() {
  media::Limits limits;
  limits.max_subscribers = 1;
  limits.max_streams = 1;
  media::Registry registry(limits);
  pb::OpenStream http;
  http.mutable_http()->set_url("http://127.0.0.1/example");
  fails(grpc::StatusCode::UNIMPLEMENTED, [&] { registry.open(http); });
  auto speech = open_request();
  speech.add_speech()->set_model_id("tiny");
  fails(grpc::StatusCode::UNIMPLEMENTED, [&] { registry.open(speech); });
  fails(grpc::StatusCode::INVALID_ARGUMENT, [&] { registry.open({}); });
  const auto opened = registry.open(open_request());
  check(opened.producer().build_id().size() == 64, "missing build identity");
  check(opened.producer().configuration_sha256().size() == 32, "missing configuration identity");
  fails(grpc::StatusCode::RESOURCE_EXHAUSTED, [&] { registry.open(open_request()); });
  auto sub = registry.subscribe({});
  fails(grpc::StatusCode::RESOURCE_EXHAUSTED, [&] { registry.subscribe({}); });
  sub->cancel();
  // Stopped does not free a slot while the RPC still owns the mailbox.
  fails(grpc::StatusCode::RESOURCE_EXHAUSTED, [&] { registry.subscribe({}); });
  sub.reset();
  sub = registry.subscribe({});
  check(!sub->stopped(), "released subscription slot not reusable");
}

class RunningServer {
 public:
  media::Registry registry;
  media::Service service{registry};
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<pb::MediaService::Stub> client;
  RunningServer() {
    if (const char* target = std::getenv("MEDIA_TEST_TARGET"); target && *target) {
      auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
      check(channel->WaitForConnected(std::chrono::system_clock::now() + 5s), "external test server unavailable");
      client = pb::MediaService::NewStub(channel);
      return;
    }
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    check(server != nullptr && port != 0, "test server failed to bind");
    client = pb::MediaService::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  }
  ~RunningServer() { if (server) server->Shutdown(std::chrono::system_clock::now() + 1s); }
};

void wire_streaming_and_integrity() {
  RunningServer running;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 5s);
  auto stream = running.client->Ingest(&context);
  pb::IngestRequest request;
  *request.mutable_open() = open_request();
  check(stream->Write(request), "open write failed");
  pb::IngestResponse response;
  check(stream->Read(&response) && response.event().has_opened(), "missing handshake before input");
  const auto id = response.event().cursor().stream_id();
  const std::string data("\x00\xff\x01media", 8);
  request.Clear();
  *request.mutable_chunk() = chunk(1, data);
  check(stream->Write(request), "chunk write failed");
  // Do not finish until the response arrives: catches buffering-until-EOF.
  check(stream->Read(&response) && response.event().has_source(), "source did not stream before finish");
  check(response.event().source().data() == data, "source bytes changed");
  check(response.event().source().sha256() == media::sha256(data), "chunk checksum wrong");
  request.Clear();
  request.mutable_finish()->set_chunk_count(1);
  request.mutable_finish()->set_byte_count(data.size());
  request.mutable_finish()->set_sha256(media::sha256(data));
  check(stream->Write(request), "finish write failed");
  stream->WritesDone();
  check(stream->Read(&response) && response.event().has_closed(), "missing terminal event");
  check(response.event().closed().status_code() == 0, "successful stream reported failure");
  check(response.event().closed().source_sha256() == media::sha256(data), "full digest wrong");
  check(!stream->Read(&response), "events after terminal");
  check(stream->Finish().ok(), "successful ingest failed");

  grpc::ClientContext subscriber_context;
  subscriber_context.set_deadline(std::chrono::system_clock::now() + 5s);
  pb::SubscribeRequest subscribe;
  subscribe.add_stream_ids(id);
  subscribe.mutable_hydration()->set_metadata(true);
  auto listener = running.client->Subscribe(&subscriber_context, subscribe);
  pb::SubscribeResponse hydrated;
  check(listener->Read(&hydrated) && hydrated.has_snapshot_begin(), "wire hydration missing begin");
  check(listener->Read(&hydrated) && hydrated.has_snapshot(), "wire hydration missing state");
  check(hydrated.snapshot().has_closed(), "closed stream not hydratable");
  check(hydrated.snapshot().closed().source_byte_count() == data.size(), "hydrated accounting wrong");
  check(listener->Read(&hydrated) && hydrated.has_snapshot_end(), "wire hydration missing end");
  subscriber_context.TryCancel();
  while (listener->Read(&hydrated)) {}
  check(listener->Finish().error_code() == grpc::StatusCode::CANCELLED, "subscription cancellation failed");
}

void malformed_wire_input() {
  RunningServer running;
  for (int scenario = 0; scenario < 6; ++scenario) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    auto stream = running.client->Ingest(&context);
    pb::IngestRequest request;
    *request.mutable_open() = open_request();
    check(stream->Write(request), "malformed test open failed");
    pb::IngestResponse response;
    check(stream->Read(&response) && response.event().has_opened(), "malformed test handshake failed");
    request.Clear();
    if (scenario == 0) {
      *request.mutable_chunk() = chunk(2, "bad-order");
    } else if (scenario == 1) {
      *request.mutable_chunk() = chunk(1, "bad-hash");
      request.mutable_chunk()->set_sha256("wrong");
    } else if (scenario == 2) {
      request.mutable_finish()->set_byte_count(1);
    } else if (scenario == 3) {
      request.mutable_finish()->set_sha256(std::string(32, 'x'));
    } else if (scenario == 4) {
      *request.mutable_open() = open_request();
    }
    if (scenario != 5) check(stream->Write(request), "malformed test write failed");
    stream->WritesDone();
    check(stream->Read(&response) && response.event().has_closed(), "failure omitted terminal event");
    check(response.event().closed().source_byte_count() == 0, "invalid bytes were accepted");
    check(!stream->Read(&response), "failure produced events after close");
    const auto status = stream->Finish();
    check(status.error_code() == (scenario == 5 ? grpc::StatusCode::CANCELLED : grpc::StatusCode::INVALID_ARGUMENT), "wrong wire failure status");
  }
}

int main() {
  try {
    uuid_selection_and_future_streams();
    snapshot_live_atomicity();
    replay_eviction_and_filtering();
    subscriber_backpressure_isolation();
    bounded_registry_and_capabilities();
    wire_streaming_and_integrity();
    malformed_wire_input();
    std::cout << "7 media protocol tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
