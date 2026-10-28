# whisper-grpc

Typed live audio/video ingestion and speech processing over gRPC.

**Status: transport implementation in progress, not a complete media/Whisper
service.** The C++ server currently implements direct byte-stream ingress,
UUID subscriptions, bounded in-memory hydration, checksums and lifecycle
events. HTTP input and speech requests fail explicitly with UNIMPLEMENTED.
Native FFmpeg decoding, Whisper workers and smart chunking are still pending.
The intended media path uses native libraries with no JSON bridge,
shell-based media processing, or request-file writes.

See [DESIGN.md](DESIGN.md) for the required behavior and acceptance tests,
and [media.proto](proto/ai/pipestream/media/v1/media.proto) for the initial
ingress, subscription, lifecycle, source-byte and speech-result messages.

Subscriptions accept an optional UUID list. An empty list follows all
authorized streams on the machine, including future streams. Optional
hydration sends bounded retained state/events before live delivery; it
does not imply durable replay or unlimited history.

Validate the draft contract with `buf lint` and `buf build`.

## Build and test the transport

Install a C++20 compiler, CMake, gRPC C++ (including the reflection library
and protoc plugin), protobuf and OpenSSL development packages. Use a protoc
version matching the protobuf libraries on the machine.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/whisper-media-server
```

On a machine with multiple protobuf installations, configure with
`-DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc` when that is the compiler
matching the installed development libraries.

The server binds to `127.0.0.1:50072` by default. Set `WHISPER_MEDIA_LISTEN`
to override it. `WHISPER_MACHINE_ID` accepts a non-nil lowercase UUID for
stable machine identity; otherwise identity is generated at startup.
The server epoch always changes on startup. Health and reflection are enabled.
The current transport is plaintext and unauthenticated: keep it on loopback
or behind an authenticated private gateway. Global subscriptions see all
streams in this server, not other server processes.

`docker compose up --build` builds and starts the transport in a non-root,
read-only container. This does not yet start a Whisper model worker.

To exercise the image using the same live wire tests (requires `grpcurl` and
the locally built `build/media_tests`):

```sh
docker build -t local/whisper-grpc:transport .
sh scripts/smoke-test.sh local/whisper-grpc:transport
```

The smoke driver starts a temporary container with a read-only filesystem,
non-root user and no volume mounts, runs direct-ingress and hydration tests,
checks gRPC health, then removes the container. Its shell code is test tooling,
not part of request processing.

Limits are currently 256 KiB per source chunk, 1 MiB history per stream,
4 MiB queued data per subscriber, 64 registered streams per server epoch,
and 64 simultaneous subscribers. Budgets charge serialized size plus an
allowance per queue entry; they are not exact RSS limits. Closed streams
currently consume registry slots until restart. Automatic history expiry,
configurable quotas and long-running registry lifecycle remain follow-up work.

The integration test uses a real loopback gRPC server. It requires the server
to return a chunk before the client sends its finish message, reconstructs
binary source data, and checks full SHA-256 accounting. Additional tests cover
concurrent snapshot/live transitions, future-stream selectors, replay eviction,
slow subscribers, cancellation and malformed lifecycle messages. These tests
do not establish media decoding or speech-recognition correctness.
