# whisper-grpc

Typed live audio/video ingestion and speech processing over gRPC.

**Status: media implementation in progress, not a complete Whisper service.**
The C++ server implements direct byte-stream ingress, UUID subscriptions,
bounded in-memory hydration, checksums, lifecycle events and native FFmpeg
demux/audio decoding. Configured ingress routes audio through fixed or VAD-based
windows to a separate native Whisper worker and publishes raw hypotheses plus
reconciled transcript revisions.
The `Pull` RPC reads permitted HTTP sources through the same pipeline. Revisions
are delivered live and retained in bounded hydration state.
The media path calls native libraries with no JSON bridge, shell-based media
processing, or request-file writes.

The standalone speech-window engine is implemented and tested for fixed cuts,
confirmed pauses, forced overlap, bounded continuous speech, final flush and
timeline discontinuities. A native Silero adapter now supplies real VAD
probabilities, with deterministic 512-sample framing independent of transport
chunks. Their combined tests verify sample coverage and pause selection on
reference speech and silence, including live ingress routing. This does not
establish accuracy on arbitrary recordings or music.

The native CPU Whisper adapter is built by default (`MEDIA_BUILD_SPEECH=OFF`
omits it). Its source is pinned by commit and archive checksum. It loads model
bytes in memory, fingerprints the exact weights, and returns native segment
text, token IDs, probabilities, language and timestamps. Adapter results remain
provisional; ingress publishes separately reconciled revisions.
The separate `SpeechWorkerService.Transcribe` RPC invokes this adapter and is
called by configured media ingress.

To run the separate real-model qualification, configure with
`-DMEDIA_TEST_MODEL=/absolute/path/to/ggml-tiny.en.bin`, rebuild, then run
`ctest --test-dir build -R speech_tests --output-on-failure`. This uses the
JFK WAV fixture from the pinned upstream source, checks recognizable reference
phrases and native metadata, and tests cancellation and model-ID rejection.
The test is not registered without a model path; ordinary test success does
not imply model qualification. Model assets are never downloaded by a request.

The reconciler keeps a bounded provisional tail and joins overlapping
lexical tokens using text and source-time evidence. It emits stable segment IDs
with increasing revisions, preserves competing text as `UNRESOLVED` when a join
is ambiguous, and stops after validation or delivery failure. Raw hypotheses
remain separate from these derived revisions. `reconcile_tests` covers repeated
words, partial-word repair, ambiguous joins, UTF-8 token boundaries, track
isolation and failed delivery. With `MEDIA_TEST_MODEL` set,
`speech_boundary_reference` checks the exact reference word sequence across
four overlapping native inference windows. This is one clean English recording,
not qualification of arbitrary accents, languages, noise or overlap.

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
(`libavformat`, `libavcodec`, `libavutil`, `libswresample`) plus libcurl development
headers and library (7.86 or newer, with asynchronous DNS). Use a protoc
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
  WHISPER_WORKER_TARGET=speech:50073 \
  docker compose --profile speech up --build
```

The worker listens on loopback port 50073, with the weights mounted read-only.
It accepts up to 32 open track/task RPCs per instance, with one inference slot.
Concurrent inference requests beyond that slot return RESOURCE_EXHAUSTED,
without an unbounded model queue. Clients must set deadlines and close unused
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

Set `WHISPER_WORKER_TARGET` on media ingress to enable speech processing.
Requests may contain up to four speech tasks and 32 track/task pairs total.
Fixed windows default to 10 seconds with one second of overlap. To enable
speech-aware requests on the native media executable, set
`WHISPER_VAD_MODEL_PATH` to the qualified model file at startup. Container
deployments must mount that file read-only and set its in-container path;
the base Compose profile does not mount VAD weights yet. Requests select
`silero-v5.1.2` and supply pause duration, padding, hard limit and no-pause
policy. The current detector threshold is 0.5. Resolved options are echoed
in the opening event. Without a worker target, speech requests fail explicitly.

Ingress publishes `SpeechHypothesis` events with window boundaries, native
worker identity and upstream window/VAD provenance. Raw hypotheses remain
provisional and may repeat overlap text. By default, ingress also publishes
`TranscriptSegment` revisions with stable IDs, increasing revision numbers and
explicit reconciliation status. Set `speech.reconcile_transcript=false` to request
raw hypotheses only. Reconciliation enables token timings in the resolved
opening options. Reconciled revisions identify the ingress reconciler, worker
model and window/VAD contributors. A final revision is immutable but can still
be `UNRESOLVED`: finalization does not assert recognition accuracy or a unique
boundary match. Remaining provisional tails flush before successful closure.
Hydration keeps the latest revision per retained segment, up to 64 segments,
sharing the retention-byte budget with raw hypotheses; eviction is explicit.
Native presentation times are applied when available;
otherwise timestamps remain on the decoded sample timeline. The latest 16
hypotheses, further bounded by the stream retention-byte budget, are hydratable;
eviction sets `result_state_truncated`. Original source bytes remain independent.
Decoded PCM can feed speech internally without publishing PCM events; set
`decode.emit_audio` to request those events too.

With both model paths configured in CMake, `pipeline_tests` runs real gRPC
ingress-to-worker tests in fixed and speech-aware modes, holds back the final
quarter of the upload until a hypothesis arrives, checks original-byte and
provenance integrity, verifies transcript revisions through a separate gRPC
subscription, and checks closed-stream hypothesis and latest-revision hydration.
The fixed-overlap case asserts the exact reference word sequence.

To qualify the packaged worker with real inference, run
`scripts/smoke-worker.sh IMAGE /path/to/ggml-tiny.en.bin /path/to/jfk.wav`
after building the local test executables. The fixture is available under the
pinned Whisper source's `samples/` directory. The driver checks native model
output over gRPC before sending finish, overload and malformed lifecycle
rejection, and a read-only non-root container with exactly one read-only model
mount. This is test tooling, not a request-path shell dependency.

To qualify the complete packaged chain, run
`scripts/smoke-pipeline.sh IMAGE /path/to/ggml-tiny.en.bin /path/to/silero.bin /path/to/jfk.wav`.
It starts separate ingress and worker containers on an isolated temporary
network, with read-only roots and model mounts, and runs the same real-model
pipeline checks against the container endpoint. The driver removes its
containers and network when it exits. `MEDIA_PIPELINE_TEST_TARGET=host:port`
can target an existing matching deployment directly from `build/pipeline_tests`.

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
4 MiB queued data per subscriber, 64 active or retained streams,
and 64 simultaneous subscribers. Budgets charge serialized size plus an
allowance per queue entry; they are not exact RSS limits. When opening a new
stream at capacity, the oldest closed stream is evicted. Active streams are
never evicted; if all slots are active, opening fails with `RESOURCE_EXHAUSTED`.
Closed streams remain hydratable until capacity pressure evicts them. Time-based
expiry and configurable quotas remain follow-up work.

A UUID still present in the registry cannot be opened twice. After eviction,
a caller may reopen that UUID with a new `stream_incarnation`. Consumers must
use `(server_epoch, stream_id, stream_incarnation, event_sequence)` as event
identity and return the complete cursor for replay. Missing or stale
incarnations fail with `OUT_OF_RANGE`, including cursors from older clients
that discard the new field. UUID subscriptions continue following future
incarnations. Already queued events retain their original incarnation and are
not rewritten or removed by registry eviction.

The integration test uses a real loopback gRPC server. It requires the server
to return a chunk before the client sends its finish message, reconstructs
binary source data, and checks full SHA-256 accounting. Additional tests cover
concurrent snapshot/live transitions, future-stream selectors, replay eviction,
slow subscribers, cancellation and malformed lifecycle messages. These tests
include native WAV/NUT media decoding but do not establish speech-recognition
correctness.

## HTTP input

Use `MediaService.Pull` with `PullRequest.open.http.url` and the same decode
and speech options as direct ingestion. HTTP is not an alternative source on
the bidirectional `Ingest` RPC. Pull returns an opening event, sanitized HTTP
metadata, numbered original-byte chunks, requested derived results, and an
explicit close with byte counts and SHA-256. Cancellation terminates the pull.
Response EOF supplies the finish boundary; the caller sends no source chunks
or finish message on this server-streaming RPC.

Configure `WHISPER_HTTP_ORIGINS` as a comma-separated list of exact canonical
origins, for example `https://media.example.org:443,http://camera.internal:8080`.
An empty list denies HTTP input. Private/local addresses also require
`WHISPER_HTTP_ALLOW_PRIVATE=1`; destination addresses are checked when libcurl
opens each socket, after DNS resolution. Proxy environment variables are not
used. TLS certificate verification remains enabled. URLs containing credentials
or fragments are rejected. Published HTTP metadata includes only origin and
status, with native libcurl provenance; paths, queries, cookies and authorization
values are not published. Credential profiles are not implemented yet.

The reader uses HTTP/1.1 and a 64 KiB application payload queue; this is not a
whole-process RSS limit. It pauses receiving during downstream processing,
so model inference is not mistaken for an HTTP source stall. HTTP transfer
framing is removed, but entity content is not decompressed. Redirects, non-2xx
responses and encoded entities such as gzip fail explicitly. Truncated declared
bodies also fail instead of appearing as successful EOF. HLS/DASH manifests,
redirect following, authentication profiles and HTTP/2/3 remain unsupported.

The HTTP tests use native local sockets and a real gRPC server. They withhold
the second half of a WAV response until decoded PCM arrives through `Pull`,
check every source byte and sample count, and verify hydrated HTTP metadata and
terminal state. Other cases cover chunked transfer framing, bounded buffering,
destination policy, cancellation, error status, redirects and truncation.

## Native media output

Without speech tasks, an absent `OpenStream.decode` selects byte-preserving relay. Set
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
