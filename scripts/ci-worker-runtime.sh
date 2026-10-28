#!/bin/sh
# Qualify the packaged worker, using the native builder only as its gRPC client.
set -eu
test "$#" -eq 3 || { echo "Usage: $0 RUNTIME_IMAGE BUILDER_IMAGE ASSET_DIRECTORY" >&2; exit 2; }
runtime=$1
builder=$2
assets=$(realpath "$3")
test -f "$assets/ggml-tiny.en.bin"
container_id=$(docker run -d --rm --read-only --cap-drop ALL --security-opt no-new-privileges:true \
  --mount "type=bind,src=$assets/ggml-tiny.en.bin,dst=/models/model.bin,readonly" \
  -e WHISPER_MODEL_PATH=/models/model.bin -e WHISPER_MODEL_ID=test-tiny.en \
  -e WHISPER_WORKER_LISTEN=0.0.0.0:50073 --entrypoint /usr/local/bin/whisper-speech-worker "$runtime")
cleanup() { docker logs "$container_id" || true; docker stop "$container_id" >/dev/null || true; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
attempt=0
until docker logs "$container_id" 2>&1 | grep -q 'speech worker listening on'; do
  test "$(docker inspect --format '{{.State.Running}}' "$container_id")" = true
  attempt=$((attempt+1))
  test "$attempt" -le 30
  sleep 1
done
docker run --rm --read-only --cap-drop ALL --security-opt no-new-privileges:true \
  --network "container:$container_id" --mount "type=bind,src=$assets,dst=/models,readonly" \
  -e MEDIA_WORKER_TEST_TARGET=127.0.0.1:50073 "$builder" \
  timeout 180 /src/build/speech_tests /models/ggml-tiny.en.bin /src/build/_deps/whisper-src/samples/jfk.wav
hardening=$(docker inspect --format 'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}} writable={{range .Mounts}}{{.RW}}{{end}}' "$container_id")
test "$hardening" = 'read_only=true user=65532:65532 mounts=1 writable=false'
printf 'Packaged worker verified: %s\n' "$hardening"
