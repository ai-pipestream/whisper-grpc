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
lexical tokens using text and source-time evidence. Native vocabulary bytes are
retained in `SpeechToken.text`; optional `rendered_text` carries the native
detokenized contribution used for matching and display. Both byte forms count
against the reconciliation memory bound. It emits stable segment IDs
with increasing revisions, preserves competing text as `UNRESOLVED` when a join
is ambiguous, and stops after validation or delivery failure. Raw hypotheses
remain separate from these derived revisions. `reconcile_tests` covers repeated
words, partial-word repair, ambiguous joins, UTF-8 token boundaries, track
isolation and failed delivery. With `MEDIA_TEST_MODEL` set,
`speech_boundary_reference` checks the exact reference word sequence across
four overlapping native inference windows. This is one clean English recording,
not qualification of arbitrary accents, languages, noise or overlap.

Multilingual functional qualification uses
`-DMEDIA_TEST_MULTILINGUAL_MODEL=/absolute/path/to/ggml-base.bin` and
`ctest --test-dir build -R translation_tests --output-on-failure`.
It runs separate Spanish transcription and English translation RPCs on a
checked-in, attributed MINDS-14 recording. The test checks language metadata,
model identity, native tokens, close accounting and three reference facts.
The base model passes this gate but still produces awkward wording (including
“knowledge account” for a known account). Tiny and base both fail the separate
synthetic Spanish challenge. See [fixture provenance and limitations](tests/fixtures/README.md)
for model/audio hashes, licensing and reproduction commands. No accuracy claim
for other recordings, accents or languages follows from this single case.

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

### Alternative native speech backend qualification

The pinned whisper.cpp source includes the native Parakeet C API. An opt-in
`parakeet_probe` target checks buffer-based model loading, cancellation recovery, reference transcription,
native token metadata, native detokenization and typed transcript reconciliation.
This is qualification tooling, not an implemented
gRPC backend: service discovery continues to advertise only the loaded Whisper
model. Parakeet transcription must not be advertised as translation support.

Candidate weights: `ggml-parakeet-tdt-0.6b-v3-q8_0.bin` from
[ggml-org/parakeet-GGUF at revision 35156454d1a39de06863303dd209fd2bed6ee079](https://huggingface.co/ggml-org/parakeet-GGUF/tree/35156454d1a39de06863303dd209fd2bed6ee079),
SHA-256 `4d64e9e96c2792186d072fde0034df0ad670cf680a2f53069052ead827fd600e`.
The original [NVIDIA Parakeet TDT 0.6B v3 model card](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3)
declares CC BY 4.0; the conversion repository labels itself MIT. Preserve NVIDIA
attribution and the original model terms for the weights, independently of
library or conversion-code licenses. Weights are not bundled or redistributed.
This pinned Q8 candidate passed the native AMD64 probe, including cancellation
followed by reuse of the same state and the exact English reference words.
This does not qualify other recordings, languages, ARM execution or a gRPC adapter.

```sh
cmake -S . -B build
cmake --build build --target parakeet_probe -j2
./build/parakeet_probe /path/to/ggml-parakeet-tdt-0.6b-v3-q8_0.bin /path/to/jfk.wav
```

### Native image and Whisper qualification

The native CPU CI matrix uses separate AMD64 and ARM64 runners without QEMU.
Run `sh scripts/ci-native.sh` locally to perform the same image build and live
runtime wire tests. It checks the host, Docker daemon and image architectures,
runs the model-independent tests in the build stage, then exercises the runtime
as UID 65532 with a read-only filesystem and no mounts. The workflow must run on
each architecture before claiming that architecture is verified.

Each native CI job then fetches revision-pinned, SHA-256-checked tiny English,
multilingual base and Silero models and runs six model-backed executables:
VAD, overlap reconciliation, worker inference, pipeline cancellation, the full
audio/video speech pipeline, and Spanish transcription/English translation.
The last test is a functional gate on one attributed human recording, not a
broad model accuracy benchmark. To run this qualification locally after the
native image build:

```sh
sh scripts/fetch-test-models.sh build/qualification-models
sh scripts/ci-models.sh local/whisper-grpc:ci-builder-amd64 build/qualification-models
```

Use `ci-builder-arm64` after a native ARM build. The scripts are explicit test
setup only. Model files remain outside images and are mounted read-only during
qualification. Services never fetch models or run these scripts. The reference
JFK recording is supplied by the pinned whisper.cpp archive and checked by hash.

CI also runs `scripts/ci-worker-runtime.sh RUNTIME_IMAGE BUILDER_IMAGE ASSET_DIRECTORY`
to verify real inference, overload handling and lifecycle behavior against the
packaged worker. The worker runs as UID 65532 with a read-only filesystem and
one read-only model mount; the builder supplies only the gRPC test client.

## Image publication

`.github/workflows/publish.yml` runs qualification for pull requests and pushes
to main. For the canonical GitHub repository, a successful main push publishes
`pipestreamai/whisper-grpc:latest` and `:sha-<full-commit>`. Manual dispatch tests
by default; its `publish` option only publishes when dispatched on main. Both
native architecture jobs must pass the runtime and model gates before release
tags are created. Each job uploads the digest of its tested image; the final
job verifies both architectures and combines those digests without rebuilding.
An older run skips publication if main has advanced. Qualification run tags
are separate from release tags. Models are never included in published images.

Publication needs the organization `DOCKER_USER` and `DOCKER_TOKEN` secrets
and Docker Hub access to `pipestreamai/whisper-grpc`. Forgejo remains canonical;
the workflow must reach GitHub through the approved mirror workflow. These
files prepare publication but do not by themselves prove that images have been
published. Run `sh tests/release_tests.sh` to check manifest rejection gates
locally with a fake Docker CLI and no registry writes.

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

`MediaService.GetServiceInfo` returns typed instance identity, registry epoch,
source chunk and retention limits, subscriber queue and stream limits, and
whether HTTP pulling or speech processing is configured. These flags describe
configuration, not downstream readiness; each HTTP URL still requires origin
validation. The optional `UiInfo` field is absent because no web UI is bundled.
`SpeechWorkerService.GetModelInfo` reports the loaded model ID and weights hash,
application and native library versions, CPU thread count, input sample rate,
window and concurrency limits, and supported tasks. English-only weights
advertise transcription only; multilingual weights also advertise translation
to English. Discovery does not allocate a stream or run inference and remains
available when all worker stream slots are occupied.

The worker cancels a stream after 120 seconds waiting for an incoming message,
including its initial opening message. Set `WHISPER_WORKER_INPUT_IDLE_SECONDS`
to an integer from 1 to 3600 to change this policy. The timer runs only while
waiting for input, so active inference is excluded. An idle expiry cancels the
RPC and releases its stream slot; it is not a successful stream close. The
resolved timeout is exposed by `GetModelInfo.input_idle_timeout_ms`. For live
sources, choose a timeout longer than the maximum time needed to accumulate
and deliver an inference window, including expected feed stalls.
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

For the complete CPU speech stack, including smart chunking, use the standalone
`compose.speech.yaml`. It wires ingress to the worker and mounts both models
read-only; no worker address or extra Compose overlay is required:

```sh
WHISPER_MODEL_PATH=/absolute/path/to/ggml-tiny.en.bin \
WHISPER_VAD_MODEL_PATH=/absolute/path/to/ggml-silero-v5.1.2.bin \
  docker compose -f compose.speech.yaml up --build
```

Both paths must name existing files; missing paths fail without creating host
directories. Models are provisioned before startup, never downloaded by a
request. `WHISPER_IMAGE` selects a previously built image for `up --no-build`;
the default local image is built from this checkout, not a published release.
`WHISPER_MEDIA_PORT` and `WHISPER_WORKER_PORT` override the loopback host ports.
Service startup order does not imply model readiness: wait for each service's
gRPC health response before sending work.

The worker listens on loopback port 50073, with the weights mounted read-only.
It accepts up to 32 open track/task RPCs per instance, with one inference slot.
Concurrent inference requests beyond that slot return RESOURCE_EXHAUSTED,
without an unbounded model queue. Clients must set deadlines and close unused
streams. Input-idle expiry defaults to 120 seconds as described above; a larger
model pool is not implemented yet.
Translation requires multilingual Whisper weights. English-only `.en` models
reject translation requests before the worker handshake; they cannot produce
results labeled as translations. With no language hint, English-only weights
use English directly; language auto-detection is reserved for multilingual
weights. A multilingual base-model gRPC check passes the core facts in one
human Spanish recording; this is not broad translation-accuracy qualification.
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
`WHISPER_VAD_MODEL_PATH` to the qualified model file at startup. The complete
`compose.speech.yaml` deployment mounts that file read-only and sets its
in-container path. The transport-only `compose.yaml` does not mount VAD weights. Requests select
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
Fixed-overlap cases assert the exact reference word sequence for WAV,
Matroska carrying PCM speech plus MJPEG video, and fragmented MP4 carrying
AAC speech plus H.264 video. Speech-aware Matroska and MP4 cases also exercise
VAD, video packet counts and hydrated final revisions; these cases check
recognition of the reference speech but do not assert every word. This is
qualification of one English recording through these container/codec paths,
not HEVC support or a general speech/video accuracy benchmark.

The same executable also runs a local HTTP-to-speech integration case with a
native loopback HTTP server and H.264/AAC video. The server withholds the final
quarter of the response until a Whisper hypothesis arrives. A global subscriber
registered before the stream exists must receive its opening event, every
transcript revision and closure. The test verifies the exact reference words,
source digest, model provenance, HTTP metadata and final transcript hydration.
This local HTTP case runs locally even when `MEDIA_PIPELINE_TEST_TARGET` points
the direct-ingress cases at an external deployment.

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
include native WAV/NUT/Matroska/fragmented-MP4 media decoding but do not establish speech-recognition
correctness.

Cancellation coverage includes two simultaneous subscribers: cancelling one
leaves ingestion and the other listener running. Cancelling ingestion publishes
a terminal cancellation event with the accepted source byte count and hash.
With `MEDIA_TEST_MODEL` configured, `pipeline_cancellation_tests` holds 31 worker
streams open, proves ingress occupies the final slot, then cancels ingress and
opens a replacement worker stream. This verifies downstream resource release
using real gRPC calls and a loaded model, without requiring inference.

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
verify PCM WAV, NUT containing PCM audio plus raw RGB video, and live
Matroska containing PCM audio plus compressed MJPEG video. The Matroska fixture
is encoded and muxed in memory using native FFmpeg APIs. Tests compare every
compressed video packet with the encoder output and verify presentation times,
audio sample counts and original container bytes. Codec
availability depends on the native FFmpeg build. The live gRPC tests hold back
the final half of each input until decoded PCM arrives, verify all output
samples and preserve the complete original source digest.

Fragmented MP4 qualification covers H.264 video plus 48 kHz mono AAC audio,
generated entirely in memory with native libx264/AAC encoders and the MP4
muxer. Tests verify all 60 video frames and their relative timestamps, codec
initialization data, continuous 16 kHz decoded audio, bounded AAC padding,
non-silent signal energy, and byte-identical source reconstruction over gRPC.
This does not qualify backward-seeking MP4, HEVC, HLS or DASH. The separate
pipeline test covers one English speech recording encoded as AAC, as described
above; it does not establish general AAC speech accuracy. Fixture encoders are test
dependencies; the service does not encode or transcode the source video.

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
