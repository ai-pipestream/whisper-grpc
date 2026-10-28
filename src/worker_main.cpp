#include "worker.h"
#include "parakeet_model.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>

int main() {
  try {
    const char* path = std::getenv("WHISPER_MODEL_PATH");
    const char* id = std::getenv("WHISPER_MODEL_ID");
    if (!path || !id || !*id) throw std::runtime_error("WHISPER_MODEL_PATH and WHISPER_MODEL_ID are required");
    const char* backend_raw = std::getenv("WHISPER_BACKEND");
    const std::string_view backend = backend_raw ? backend_raw : "whisper";
    if (backend != "whisper" && backend != "parakeet") throw std::runtime_error("WHISPER_BACKEND must be whisper or parakeet");
    int threads = 4;
    int idle_seconds = 120;
    if (const char* raw = std::getenv("WHISPER_WORKER_INPUT_IDLE_SECONDS")) {
      const std::string_view text(raw);
      const auto result = std::from_chars(text.data(), text.data() + text.size(), idle_seconds);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || idle_seconds < 1 || idle_seconds > 3600)
        throw std::runtime_error("WHISPER_WORKER_INPUT_IDLE_SECONDS must be an integer from 1 to 3600");
    }
    if (const char* raw = std::getenv("WHISPER_THREADS")) {
      const std::string_view text(raw);
      const auto result = std::from_chars(text.data(), text.data() + text.size(), threads);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("invalid WHISPER_THREADS");
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (!file || length <= 0 || length > INT64_C(2147483648))
      throw std::runtime_error("model must be a readable nonempty file no larger than 2 GiB");
    std::string weights(static_cast<size_t>(length), '\0');
    file.seekg(0);
    if (!file.read(weights.data(), length)) throw std::runtime_error("cannot read model weights");
    std::unique_ptr<media::SpeechBackend> model;
    if (backend == "parakeet") model = std::make_unique<media::ParakeetModel>(weights, id, threads);
    else model = std::make_unique<media::SpeechModel>(weights, id, threads);
    std::string().swap(weights);
    const char* machine = std::getenv("WHISPER_MACHINE_ID");
    media::SpeechWorker service(*model, machine ? machine : "", std::chrono::seconds(idle_seconds));
    const char* address = std::getenv("WHISPER_WORKER_LISTEN");
    if (!address) address = "127.0.0.1:50073";
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(2 * 1024 * 1024);
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) throw std::runtime_error("cannot bind speech worker");
    std::cerr << "speech worker listening on " << address << '\n';
    server->Wait();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
