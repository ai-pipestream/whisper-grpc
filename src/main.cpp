#include "service.h"

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
    media::Service service(registry);
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
