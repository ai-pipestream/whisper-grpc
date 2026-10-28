#!/bin/sh
# Developer driver only. Request processing never invokes this script.
set -eu
if [ "$#" -ne 3 ]; then
  printf 'Usage: %s IMAGE TINY_EN_MODEL JFK_WAV\n' "$0" >&2
  exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
image=$1
model=$(realpath "$2")
fixture=$(realpath "$3")
container_id=$(docker run -d --rm --read-only --cap-drop ALL \
  --security-opt no-new-privileges:true -p 127.0.0.1::50073 \
  --mount "type=bind,src=$model,dst=/models/model.bin,readonly" \
  -e WHISPER_WORKER_LISTEN=0.0.0.0:50073 -e WHISPER_MODEL_PATH=/models/model.bin \
  -e WHISPER_MODEL_ID=test-tiny.en --entrypoint /usr/local/bin/whisper-speech-worker "$image")
trap 'docker stop "$container_id" >/dev/null' EXIT HUP INT TERM
endpoint=$(docker port "$container_id" 50073/tcp)
ready=false
attempt=0
while [ "$attempt" -lt 30 ]; do
  if grpcurl -max-time 1 -plaintext -format text -import-path "$root/tests/proto" -proto health.proto \
      "$endpoint" grpc.health.v1.Health/Check 2>/dev/null | grep -q 'status: SERVING'; then
    ready=true
    break
  fi
  attempt=$((attempt + 1))
  sleep 1
done
if [ "$ready" != true ]; then
  docker logs "$container_id" >&2
  exit 1
fi
MEDIA_WORKER_TEST_TARGET="$endpoint" "$root/build/speech_tests" "$model" "$fixture"
hardening=$(docker inspect --format \
  'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}} writable={{range .Mounts}}{{.RW}}{{end}}' "$container_id")
test "$hardening" = 'read_only=true user=65532:65532 mounts=1 writable=false'
printf '%s\n' "$hardening"
