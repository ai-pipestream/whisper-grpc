#include "service.h"
#ifdef MEDIA_HAS_SPEECH
#include "pipeline.h"
#include <fstream>
#endif

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include <cstdlib>
#include <iostream>

int main() {
  try {
    const char* address = std::getenv("WHISPER_MEDIA_LISTEN");
    const char* machine = std::getenv("WHISPER_MACHINE_ID");
    media::Registry registry({}, machine ? machine : "");
    std::unique_ptr<media::ProcessingFactory> processing;
#ifdef MEDIA_HAS_SPEECH
    if (const char* target = std::getenv("WHISPER_WORKER_TARGET"); target && *target) {
      std::string vad;
      if (const char* path = std::getenv("WHISPER_VAD_MODEL_PATH"); path && *path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto length = file.tellg();
        if (!file || length <= 0 || length > 4 * 1024 * 1024) throw std::runtime_error("invalid VAD model file");
        vad.resize(static_cast<size_t>(length)); file.seekg(0);
        if (!file.read(vad.data(), length)) throw std::runtime_error("cannot read VAD weights");
      }
      processing = std::make_unique<media::SpeechPipelineFactory>(target, std::move(vad), registry.machine_id());
    }
#endif
    media::HttpPolicy http;
    if (const char* raw = std::getenv("WHISPER_HTTP_ORIGINS"); raw && *raw) {
      std::string origins(raw);
      size_t start = 0;
      do {
        const auto end = origins.find(',', start);
        const auto item = origins.substr(start, end == std::string::npos ? end : end - start);
        if (item != media::HttpInput::origin(item)) throw std::runtime_error("HTTP origins must use canonical scheme://host:port form");
        http.allowed_origins.insert(item);
        if (end == std::string::npos) break;
        start = end + 1;
      } while (true);
    }
    if (const char* raw = std::getenv("WHISPER_HTTP_ALLOW_PRIVATE")) {
      if (std::string(raw) != "0" && std::string(raw) != "1") throw std::runtime_error("WHISPER_HTTP_ALLOW_PRIVATE must be 0 or 1");
      http.allow_private_addresses = std::string(raw) == "1";
    }
    media::Service service(registry, processing.get(), std::move(http));
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(512 * 1024);
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);
    builder.AddListeningPort(address ? address : "127.0.0.1:50072", grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) throw std::runtime_error("cannot bind media server");
    std::cerr << "media server listening on " << (address ? address : "127.0.0.1:50072")
              << "; epoch=" << registry.epoch() << '\n';
    server->Wait();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
