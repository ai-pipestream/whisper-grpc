# whisper-grpc

Typed live audio/video ingestion and speech processing over gRPC.

**Status: media implementation in progress, not a complete Whisper service.**
The C++ server implements direct byte-stream ingress, UUID subscriptions,
bounded in-memory hydration, checksums, lifecycle events and native FFmpeg
demux/audio decoding. HTTP input and speech requests still fail explicitly
with UNIMPLEMENTED. A separate native Whisper worker is implemented; ingress
routing to it and service-level VAD integration remain pending.
The media path calls native libraries with no JSON bridge, shell-based media
processing, or request-file writes.

The standalone speech-window engine is implemented and tested for fixed cuts,
confirmed pauses, forced overlap, bounded continuous speech, final flush and
timeline discontinuities. A native Silero adapter now supplies real VAD
probabilities, with deterministic 512-sample framing independent of transport
chunks. Their combined tests verify sample coverage and pause selection on
reference speech and silence. They are not yet exposed through the media
service; this does not establish accuracy on arbitrary recordings or music.

The native CPU Whisper adapter is built by default (`MEDIA_BUILD_SPEECH=OFF`
omits it). Its source is pinned by commit and archive checksum. It loads model
bytes in memory, fingerprints the exact weights, and returns native segment
text, token IDs, probabilities, language and timestamps. Adapter results remain
provisional: cross-window reconciliation and finalization remain pending.
The separate `SpeechWorkerService.Transcribe` RPC invokes this adapter;
it is not yet called by the media server.

To run the separate real-model qualification, configure with
`-DMEDIA_TEST_MODEL=/absolute/path/to/ggml-tiny.en.bin`, rebuild, then run
`ctest --test-dir build -R speech_tests --output-on-failure`. This uses the
JFK WAV fixture from the pinned upstream source, checks recognizable reference
phrases and native metadata, and tests cancellation and model-ID rejection.
The test is not registered without a model path; ordinary test success does
not imply model qualification. Model assets are never downloaded by a request.

Real VAD qualification additionally uses `-DMEDIA_TEST_VAD_MODEL=/path/to/model.bin`
and `ctest --test-dir build -R vad_tests --output-on-failure`. The currently
qualified weights are [Silero v5.1.2 in GGML format](https://huggingface.co/ggml-org/whisper-vad/resolve/9ffd54a1e1ee413ddf265af9913beaf518d1639b/ggml-silero-v5.1.2.bin),
SHA-256 `29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf`.
Other weights are rejected until their input framing is qualified. The adapter
loads weights through an in-memory native loader, maintains state per track,
and emits only real samples, excluding final model padding. Detection remains
probabilistic. Tests cover reference speech at original and attenuated volume,
silence, invalid PCM, transport-framing invariance,
pause-based windowing and discontinuities, not a broad acoustic benchmark.

`cmake/PatchWhisperVad.cmake` fixes the pinned upstream compute-error path to
return failure instead of success with an untouched probability. Configuration
fails if the exact pinned source no longer matches the patch. This patch is
included in the service build fingerprint.

See [DESIGN.md](DESIGN.md) for the required behavior and acceptance tests,
and [media.proto](proto/ai/pipestream/media/v1/media.proto) for the initial
ingress, subscription, lifecycle, source-byte and speech-result messages.

Subscriptions accept an optional UUID list. An empty list follows all
authorized streams on the machine, including future streams. Optional
hydration sends bounded retained state/events before live delivery; it
does not imply durable replay or unlimited history.

Validate the draft contract with `buf lint` and `buf build`.

## Build and test

Install a C++20 compiler, CMake, gRPC C++ (including the reflection library
and protoc plugin), protobuf, OpenSSL and FFmpeg development packages
(`libavformat`, `libavcodec`, `libavutil`, `libswresample`). Use a protoc
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
read-only container. To also start the separate CPU worker, provide an existing
model file and enable its profile:

```sh
WHISPER_MODEL_PATH=/absolute/path/to/ggml-tiny.en.bin \
  docker compose --profile speech up --build
```

The worker listens on loopback port 50073, with the weights mounted read-only.
It accepts one active RPC per model instance and returns RESOURCE_EXHAUSTED
for excess concurrent streams. Clients must set deadlines and close unused
streams; idle-stream expiry and a larger model pool are not implemented yet.
No request media is saved to disk. `WHISPER_MODEL_ID` defaults to `tiny.en`,
and `WHISPER_THREADS` defaults to four. The standalone executable uses the
same settings and defaults to `127.0.0.1:50073`.

Worker clients open one source UUID/track with model options, send complete
mono 16 kHz float32 windows (at most 30 seconds each), and explicitly finish
with a matching window count. Windows identify their overlap and cut reason.
The worker applies no extra overlap. Its output is an unreconciled hypothesis
per window; overlapping hypotheses can repeat text. Close means all supplied
windows completed, not that a final combined transcript exists. Its handshake
reports the actual model hash, build, native library and settings digest.

To qualify the packaged worker with real inference, run
`scripts/smoke-worker.sh IMAGE /path/to/ggml-tiny.en.bin /path/to/jfk.wav`
after building the local test executables. The fixture is available under the
pinned Whisper source's `samples/` directory. The driver checks native model
output over gRPC before sending finish, overload and malformed lifecycle
rejection, and a read-only non-root container with exactly one read-only model
mount. This is test tooling, not a request-path shell dependency.

To exercise the image using the same live wire tests (requires `grpcurl` and
the locally built `build/media_tests`):

```sh
docker build -t local/whisper-grpc:transport .
sh scripts/smoke-test.sh local/whisper-grpc:transport
```

The smoke driver starts a temporary container with a read-only filesystem,
non-root user and no volume mounts, runs direct-ingress, live decode and hydration tests,
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
include native WAV/NUT media decoding but do not establish speech-recognition
correctness.

## Native media output

An absent `OpenStream.decode` selects byte-preserving relay. Set
`decode.emit_audio` to stream mono 16 kHz float32 little-endian PCM, and/or
`decode.emit_packets` for original demuxed audio/video/subtitle/data packets.
An empty `decode.audio_track_ids` selects all audio tracks; an explicit list
selects container track indexes. Supplying a decode message also emits typed
container and track descriptions. `SourceChunk` events continue to carry the
exact original container bytes, independently of these projections.

Packet fragments retain a packet number, track index, byte offset, total
packet length, source position when available, native PTS/DTS and rational
time base, flags and side data. Video packets are not transcoded or claimed
to be independently playable segments. Metadata includes codec initialization
bytes, source audio/video dimensions and native dictionary entries. The source
bytes preserve native fields that this metadata projection does not yet expose.

FFmpeg pulls directly from gRPC through a custom in-memory AVIO reader. The
decoder never receives a filesystem path. Nested file/network opens by demuxers
are rejected. Decoder provenance records the actual linked library versions
and a configuration digest incorporating the accepted decode options.

The allowlist currently includes WAV, MP3, FLAC, Ogg, MOV/MP4, Matroska/WebM,
MPEG-TS, AAC and NUT. This is a decoder allowlist, not a claim that every codec
and container combination has passed qualification. Current generated fixtures
verify PCM WAV and NUT containing PCM audio plus raw RGB video. Codec
availability depends on the native FFmpeg build. The live gRPC tests hold back
the final half of each input until decoded PCM arrives, verify all output
samples and preserve the complete original source digest.

The native WAV demuxer probes 64 KiB of audio for embedded SPDIF at startup,
about two seconds for mono 16-bit 16 kHz input; shorter WAV files may need EOF
before decoding starts. WAV packet size after the probe is capped at 4096
bytes. The NUT fixture produces PCM after about 4 KiB of input. Neither figure
is a latency guarantee for other encodings. Classical MP4 requiring backward
seeks is not guaranteed to work on non-seekable input; a finite-file memory
mode has not been implemented. HLS/DASH manifests and external resources are
not supported by this direct-container decoder.

Limits include 64 tracks, a 16 MiB native packet (published as at most 128 KiB
payload fragments), bounded metadata/side data, and a process-wide 32 MiB limit
on individual FFmpeg allocations. These are per-allocation/data limits, not a
total process memory cap. Midstream audio rate/layout/format changes fail
explicitly; source timestamp discontinuities reset resampling and are flagged
on the next PCM chunk. Original media is always preserved independently.
