#include "pipeline.h"
#include "service.h"
#include "worker.h"
#include "media_fixture.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace std::chrono_literals;
namespace pb = media::pb;
void check(bool value, const char* message) {if (!value) throw std::runtime_error(message);}
struct Server {
  std::unique_ptr<grpc::Server> instance;
  int port = 0;
  explicit Server(grpc::Service& service) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service); instance = builder.BuildAndStart();
    check(instance != nullptr, "cancellation test server bind failed");
  }
  ~Server() {instance->Shutdown(std::chrono::system_clock::now()+1s); instance->Wait();}
  std::string address() const {return "127.0.0.1:"+std::to_string(port);}
};
struct Idle {
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriter<pb::TranscribeRequest,pb::TranscribeResponse>> rpc;
  explicit Idle(pb::SpeechWorkerService::Stub& stub) {
    context.set_deadline(std::chrono::system_clock::now()+30s);
    rpc = stub.Transcribe(&context);
    pb::TranscribeRequest request;
    request.mutable_open()->set_stream_id(media::uuid());
    request.mutable_open()->mutable_options()->set_model_id("test-tiny.en");
    check(rpc->Write(request), "idle worker request failed");
    pb::TranscribeResponse response;
    check(rpc->Read(&response) && response.has_opened(), "worker slot unavailable");
  }
  ~Idle() {
    context.TryCancel(); pb::TranscribeResponse response;
    while (rpc->Read(&response)) {} rpc->Finish();
  }
};
int main(int argc, char** argv) {
  try {
    check(argc==2, "usage: pipeline_cancellation_tests TINY_EN_MODEL");
    std::ifstream file(argv[1], std::ios::binary); check(file.good(), "cancellation model missing");
    const std::string weights{std::istreambuf_iterator<char>(file), {}};
    media::SpeechModel model(weights, "test-tiny.en", 4);
    media::SpeechWorker worker(model); Server worker_server(worker);
    auto worker_stub = pb::SpeechWorkerService::NewStub(grpc::CreateChannel(worker_server.address(), grpc::InsecureChannelCredentials()));
    std::vector<std::unique_ptr<Idle>> held;
    for (int i=0; i<31; ++i) held.push_back(std::make_unique<Idle>(*worker_stub));
    media::SpeechPipelineFactory factory(worker_server.address());
    media::Registry registry; media::Service service(registry, &factory); Server ingress(service);
    auto stub = pb::MediaService::NewStub(grpc::CreateChannel(ingress.address(), grpc::InsecureChannelCredentials()));
    grpc::ClientContext input_context, observer_context;
    input_context.set_deadline(std::chrono::system_clock::now()+10s);
    observer_context.set_deadline(std::chrono::system_clock::now()+10s);
    auto input = stub->Ingest(&input_context);
    pb::IngestRequest request;
    request.mutable_open()->mutable_direct()->set_name("cancel.wav");
    request.mutable_open()->mutable_decode()->set_emit_audio(true);
    auto* speech = request.mutable_open()->add_speech(); speech->set_model_id("test-tiny.en");
    speech->mutable_windowing()->set_window_ms(30000); // Open a worker, but never reach inference.
    check(input->Write(request), "cancellation ingress open failed");
    pb::IngestResponse response;
    check(input->Read(&response) && response.event().has_opened(), "cancellation handshake missing");
    pb::SubscribeRequest selection; selection.add_stream_ids(response.event().cursor().stream_id());
    selection.add_payload_kinds(pb::PAYLOAD_KIND_TRANSCRIPT); selection.mutable_hydration()->set_metadata(true);
    auto observer = stub->Subscribe(&observer_context, selection);
    pb::SubscribeResponse item;
    check(observer->Read(&item) && item.has_snapshot_begin(), "cancellation observer boundary missing");
    check(observer->Read(&item) && item.has_snapshot(), "cancellation observer state missing");
    check(observer->Read(&item) && item.has_snapshot_end(), "cancellation observer registration incomplete");
    auto prefix = wav_fixture(16000, 1, 6); prefix.resize(prefix.size()/2);
    request.Clear(); request.mutable_chunk()->set_chunk_number(1); request.mutable_chunk()->set_data(prefix);
    check(input->Write(request), "cancellation prefix write failed");
    bool audio = false;
    while (input->Read(&response)) {if (response.event().has_audio()) {audio=true;break;}}
    check(audio, "pipeline never opened its worker before cancellation");
    {
      grpc::ClientContext context; context.set_deadline(std::chrono::system_clock::now()+5s);
      auto overflow = worker_stub->Transcribe(&context); overflow->WritesDone();
      pb::TranscribeResponse output;
      check(!overflow->Read(&output) && overflow->Finish().error_code()==grpc::StatusCode::RESOURCE_EXHAUSTED,
            "pipeline did not occupy the final worker slot");
    }
    input_context.TryCancel(); while (input->Read(&response)) {}
    check(input->Finish().error_code()==grpc::StatusCode::CANCELLED, "input did not cancel");
    check(observer->Read(&item) && item.event().has_closed() &&
          item.event().closed().status_code()==grpc::StatusCode::CANCELLED &&
          item.event().closed().source_byte_count()==prefix.size() &&
          item.event().closed().source_sha256()==media::sha256(prefix), "cancelled pipeline accounting lost");
    // The other 31 calls remain live. This can succeed only if ingress released its downstream call.
    auto replacement = std::make_unique<Idle>(*worker_stub);
    observer_context.TryCancel(); while (observer->Read(&item)) {} observer->Finish();
    std::cout << "Cancelled ingress released its worker slot and preserved terminal accounting\n";
  } catch (const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
}
