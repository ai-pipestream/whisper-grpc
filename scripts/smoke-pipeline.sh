#!/bin/sh
# Developer qualification only. No script runs in the media request path.
set -eu
if [ "$#" -ne 4 ]; then
  printf 'Usage: %s IMAGE TINY_EN_MODEL SILERO_MODEL JFK_WAV\n' "$0" >&2
  exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
image=$1
model=$(realpath "$2")
vad=$(realpath "$3")
fixture=$(realpath "$4")
network=whisper-grpc-smoke-$$
worker_container=
media_container=
cleanup() {
  status=$?
  if [ "$status" -ne 0 ]; then
    [ -z "$media_container" ] || docker logs "$media_container" >&2 || true
    [ -z "$worker_container" ] || docker logs "$worker_container" >&2 || true
  fi
  [ -z "$media_container" ] || docker stop "$media_container" >/dev/null || true
  [ -z "$worker_container" ] || docker stop "$worker_container" >/dev/null || true
  docker network rm "$network" >/dev/null || true
}
docker network create "$network" >/dev/null
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
wait_ready() {
  endpoint=$1
  attempt=0
  while [ "$attempt" -lt 30 ]; do
    if grpcurl -max-time 1 -plaintext -format text -import-path "$root/tests/proto" -proto health.proto \
        "$endpoint" grpc.health.v1.Health/Check 2>/dev/null | grep -q 'status: SERVING'; then
      return 0
    fi
    attempt=$((attempt + 1))
    sleep 1
  done
  return 1
}
worker_container=$(docker run -d --rm --read-only --cap-drop ALL \
  --security-opt no-new-privileges:true --network "$network" --network-alias worker \
  -p 127.0.0.1::50073 --mount "type=bind,src=$model,dst=/models/model.bin,readonly" \
  -e WHISPER_WORKER_LISTEN=0.0.0.0:50073 -e WHISPER_MODEL_PATH=/models/model.bin \
  -e WHISPER_MODEL_ID=test-tiny.en --entrypoint /usr/local/bin/whisper-speech-worker "$image")
wait_ready "$(docker port "$worker_container" 50073/tcp)"
media_container=$(docker run -d --rm --read-only --cap-drop ALL \
  --security-opt no-new-privileges:true --network "$network" -p 127.0.0.1::50072 \
  --mount "type=bind,src=$vad,dst=/models/vad.bin,readonly" \
  -e WHISPER_WORKER_TARGET=worker:50073 -e WHISPER_VAD_MODEL_PATH=/models/vad.bin "$image")
media_endpoint=$(docker port "$media_container" 50072/tcp)
wait_ready "$media_endpoint"
MEDIA_PIPELINE_TEST_TARGET="$media_endpoint" "$root/build/pipeline_tests" "$model" "$vad" "$fixture"
for container_id in "$media_container" "$worker_container"; do
  hardening=$(docker inspect --format \
    'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}} writable={{range .Mounts}}{{.RW}}{{end}}' "$container_id")
  test "$hardening" = 'read_only=true user=65532:65532 mounts=1 writable=false'
  printf '%s\n' "$hardening"
done
