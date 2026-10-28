# Live media and speech services

Status: media implementation in progress. Direct ingestion, subscriptions,
bounded hydration, source checksums and native FFmpeg demux/audio decoding have
a C++ implementation and loopback gRPC tests. The sections below describe the
full target, including work not yet implemented. The separate speech worker
performs native inference and ingress chains to it through fixed or real VAD
windows. HTTP input and reconciliation remain pending; see README.md for
current format coverage and limits.

## Required behavior

Use C++ with the native gRPC API, FFmpeg's libavformat/libavcodec/libswresample,
and whisper.cpp's C API. Avoid a shell, ffprobe JSON, or a Python model server
in the request path. Rust remains a reasonable orchestration alternative, but
does not remove the need for these native media/model libraries.

Two independent processes keep decoding and model scheduling separate:

1. Media ingress accepts direct gRPC byte chunks or pulls an HTTP(S) source.
   It publishes original media bytes and demuxed/decoded media events.
2. A speech worker consumes timed decoded audio and emits model results.
   It must retain context across chunks, use overlap and stable segment
   revisions, and flush its final window before reporting completion.

The ingress stream can return original media, speech output, or both. Output
filters must not silently turn off processing requested by other subscribers.
The subscription contract includes initial container/track/packet descriptors.
The worker protocol now carries explicit pre-cut windows and provisional
hypotheses. Ingress publishes and hydrates bounded provisional hypotheses;
final transcript reconciliation and full native metadata coverage still need
implementation and validation.

## Speech-aware chunking and optional overlap

Offer fixed-duration and speech-aware modes. In speech-aware mode, run VAD
continuously on decoded audio, retaining detector state across transport
chunks. Prefer a confirmed non-speech interval near the target window duration.
Require a minimum pause and padding at both speech edges; only the interior
of a sufficiently long pause is eligible. Speech-aware normal cuts need no
overlap. Silence remains part of the source and decoded timeline, not discarded
data. Split on a sample-frame boundary and carry all audio forward exactly once
unless an explicitly marked fallback requires overlap.

Boundary selection has bounded lookahead. Retain the most recent eligible
candidate near the target, wait for a candidate after the target if necessary,
and cut by the configured maximum duration. Validation must account for the
detector's lookahead and padding in the total buffer/latency budget, and require
a pause long enough for both padding margins. At a maximum-duration decision,
use a confirmed candidate if one exists; otherwise apply NoPausePolicy.
NoPausePolicy either forces an overlapping split or fails speech processing.
Forced overlap must be positive, leave a bounded minimum hop, and fit within
the model context limit. Never wait indefinitely for silence or silently
truncate a continuous speaker. End of input flushes even without a final pause.

VAD estimates speech presence; it cannot guarantee a boundary contains no
dialogue. Quiet speech, music, background voices and overlapping speakers need
fixture coverage. Energy thresholds alone are insufficient. Record the VAD
model/version/settings and the boundary reason (detected pause, forced limit,
end of input or discontinuity) in the eventual worker-window contract. Expose
the fallback rather than describing every cut as speech-safe.

A speech pause is not necessarily a video keyframe. These boundaries govern
audio inference; independently playable video segments need their own
keyframe-aware muxing policy while sharing the source timeline.

Transport chunks and inference windows have different boundaries. Preserve
source bytes exactly once, and decode the container continuously across upload
chunks. Never concatenate overlapping compressed media bytes: this can corrupt
the container or repeat audio. Decoded audio is the unit of overlap.

For fixed mode, start evaluation with a 10-second window and 2 seconds of overlap:
windows cover `[0, 10)`, `[8, 18)`, `[16, 26)` seconds. These are candidate
defaults, not measured accuracy or latency guarantees. `SpeechWindowing`
specifies the requested durations; the handshake reports resolved values.
Validate `window_ms > 0` and `0 <= overlap_ms < window_ms`, model context
limits, and a server minimum hop size so excessive overlap cannot cause
unbounded inference work. Convert durations to sample counts once using a
documented rounding rule; advance by sample count to avoid accumulated drift.

Overlap is created once by the stage forming inference windows. If those
windows cross a gRPC boundary, the worker contract must carry an explicit
window identity, source track, absolute first sample, sample count and
overlap/new-audio boundary. A receiving worker must not add overlap again.
The current `AudioChunk` messages describe contiguous decoded transport;
they are not yet that window protocol.

Keep a provisional tail around the overlap boundary and reconcile it with
the next decode using source timing and token/text alignment. Only finalize
the stable prefix. A segment crossing the commit boundary stays provisional
as a whole; revisions retain the same segment UUID and increase revision.
Never deduplicate solely by identical text, since repeated words may be real
speech. Do not revise finalized segments. If no stable alignment is found
within the bounded context budget, report uncertainty instead of silently
discarding speech or growing memory without limit.

On explicit finish, decode the remaining real samples and flush provisional
results once before StreamClosed. Silence/VAD may help select boundaries but
must not remove source bytes. A timeline discontinuity resets overlap state;
audio from different tracks or streams must never share context. Padding
required by the model is not source audio and must not extend source timing.

Tests must deliberately split a spoken word across transport and inference
boundaries, verify no missing/duplicated words against reference audio, retain
legitimate repeated phrases, and cover short inputs, final flush, silence,
discontinuities, and concurrent streams. Schema validation alone cannot prove
any of these recognition properties.

Speech-aware tests must additionally cover pauses shorter than the required
margin, long silence, continuous dialogue beyond the hard limit, quiet speech,
music with speech, speech resuming during lookahead and both no-pause policies.
Assert bounded memory/latency, exact audio coverage, actual chosen boundaries
and explicit forced-cut reporting, rather than checking VAD labels alone.

## Stream selection and hydration

The user-confirmed subscription selector is an optional list of UUIDs.
Empty means every authorized stream on this machine, including future streams.
A machine runs one authoritative stream registry; multiple service processes
publish into it. A global subscription is not automatically fleet-wide.
Explicit unknown UUIDs are future subscriptions, not typos the server repairs.

Each stream has a unique UUID. Each registry incarnation has a server epoch.
An event's identity is `(server_epoch, stream_id, event_sequence)`. Source
chunk numbering, decoded audio chunk numbering and event numbering are
different counters. Payload filters leave gaps in event numbers by design.

Hydration has two independently selectable parts:

- Current state: source description, codec/track descriptors, processing
  configuration, producer and model identities, latest final/revisable results,
  lifecycle state and the bounds of available retained history.
- Retained events: exact original events still in the bounded in-memory ring.

Current-state result hydration also needs a byte/count cap; an unlimited set
of final segments is not a bounded-memory snapshot. Report truncation and its
earliest available position. Never call a partial transcript complete history.

Atomically register the subscription and freeze its per-stream high-water
marks. Send SnapshotBegin, hydrated state and selected retained events, then
SnapshotEnd, then live events beyond those marks. Events published while
hydrating must be buffered in a bounded pending queue, not lost in a gap
between snapshot and subscription. If the queue fills, terminate that
subscription with RESOURCE_EXHAUSTED and its last delivered cursor.

Reject a requested replay cursor from an old epoch or evicted history with
OUT_OF_RANGE. There is no disk-backed replay or cross-restart recovery in v1.
Closed streams remain hydratable for a bounded time/space budget; subscriptions
do not keep closed-stream state alive forever. An empty-selector subscription
stays open after any individual stream closes.

## Losslessness and metadata

Preserve original input bytes independently of demuxed packets, decoded audio,
video frames, and model results. Byte offsets, ordered chunk numbers and
SHA-256 checksums make exact reconstruction testable. Arbitrary transport
chunks are not promised to be independently playable video segments.

For HTTP, original bytes mean the response entity delivered to media ingest,
not TLS records or the entire HTTP transport. Explicitly define any content
decoding. HLS/DASH need manifest, segment, discontinuity and resource identities
preserved individually; downloading a playlist as one file is not HLS support.

Expose stream/container metadata, every track (including subtitles/data),
codec configuration/extradata, side data, source PTS/DTS and rational time
bases, durations and keyframe flags. Preserve metadata entries as repeated
entries where duplicate keys are possible. Keep unknown native metadata as
named opaque bytes when it cannot be decoded safely. Original source bytes
remain the authority for fields that the demuxer cannot expose.

Every derived result identifies service version/build, machine, model hash,
backend/library versions, resolved settings and the source timeline/range.
Wall-clock arrival time and media presentation time are distinct. Do not
fabricate timestamps for containers that omit them. HTTP credentials and
secret-bearing source URLs are excluded from shared metadata and logs.

## Diskless live processing

Use custom AVIO read callbacks over bounded in-memory queues for direct feeds.
Connect HTTP input incrementally, with deadlines and cancellation. Link the
libraries directly; do not spawn an ffmpeg or ffprobe process. Model weights
may be mounted read-only; request bytes must not be saved to a filesystem.

MPEG-TS, fragmented MP4 and suitable WebM/Ogg streams are candidates for live
decode. Classic MP4 with its index at the end is not inherently live-decodable.
Report that limitation; either reject it for strict-live operation or use an
explicitly selected bounded in-memory finite-file mode. Never advertise full
live-video support on the strength of a whole-file memfd implementation.

Bound queues in bytes, not just message counts. Apply backpressure to ingress;
disconnect a slow optional subscriber rather than blocking all others. A
source that cannot be backpressured must fail visibly if lossless capture
cannot keep up. Disconnect/cancellation must interrupt blocked I/O and release
decoder/model resources. No silent dropping, no unbounded replay promise.

HTTP destinations and redirects need operator-controlled policy, including
DNS/IP validation. Model and credential names resolve from server
configuration. Do not let callers supply arbitrary model paths or launch args.

## Existing fleet integration

`grpc-asr` already embeds whisper.cpp and exposes a typed bidirectional ASR
RPC. Its current video path waits for a sealed full-input memfd and invokes
ffmpeg children. Reuse its model-pool and timing work where appropriate, but
do not label that path live video. Its document projection is not a lossless
substitute for the native model stream.

The supplied `ai-pipestream/whisper-grpc` repository started with only a README
and Apache-2.0 license. A Forgejo repository of that name was not accessible
during initial inspection. No remote changes have been made by this work.

## Models

Whisper is a speech model. It transcribes and translates speech into English;
it does not describe video scenes or provide arbitrary-language translation.
Keep source-language transcription and translated text as separate results.

Candidate optional workers are sherpa-onnx streaming ASR/VAD and speaker
processing, plus a vision-language worker using llama.cpp/libmtmd for selected
video frames. Each model needs a verified weight license, resource budget,
typed result contract and real fixture coverage before being advertised.
Open-source inference avoids per-request API charges, not compute costs.
Video frame analysis is sampled unless the contract explicitly promises all
frames; sampling policy and coverage belong in result metadata.

Native ARM64 and x86-64 builds are required. Apple Metal acceleration requires
a native macOS build; Linux ARM64 Docker support does not imply Metal support.

## Acceptance tests before release

- Client holds the last input chunk until a decoded or speech event arrives:
  proves live processing rather than streaming a completed batch.
- Reconstruct source bytes exactly and verify digest, including unknown
  container metadata and non-audio tracks.
- Multiple UUID selectors, empty selector, new-stream arrival, closed-stream
  hydration, duplicate IDs and caller-provided UUID validation.
- Publish during snapshot construction: no missing or duplicated event at
  the hydration/live boundary, including concurrent stream creation/closure.
- Slow subscribers, bounded queue overflow, evicted cursors, restart epochs,
  cancellation while waiting for input and interrupted HTTP requests.
- Chunk reordering, duplicate chunks, offsets, checksum mismatch, truncated
  media, finish accounting, duplicate finish and disconnect without finish.
- Timestamp discontinuities and non-zero origins, multiple audio tracks,
  silence, speech crossing window boundaries, stable partial/final revisions.
- Container runs read-only; trace request processing for filesystem writes.
- Native ARM64 and x86-64 tests; actual speech output checked against known
  reference text. Mock model results are not Whisper validation.
