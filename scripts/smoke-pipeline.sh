#!/bin/sh
# Developer qualification only. No script runs in the media request path.
set -eu
if [ "$#" -ne 4 ]; then
  printf 'Usage: %s IMAGE TINY_EN_MODEL SILERO_MODEL JFK_WAV\n' "$0" >&2
  exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WHISPER_IMAGE=$1
WHISPER_MODEL_PATH=$(realpath "$2")
WHISPER_VAD_MODEL_PATH=$(realpath "$3")
fixture=$(realpath "$4")
WHISPER_MODEL_ID=test-tiny.en
WHISPER_MEDIA_PORT=0
WHISPER_WORKER_PORT=0
export WHISPER_IMAGE WHISPER_MODEL_PATH WHISPER_VAD_MODEL_PATH
export WHISPER_MODEL_ID WHISPER_MEDIA_PORT WHISPER_WORKER_PORT
project=whisper-grpc-smoke-$$
compose() { docker compose -p "$project" -f "$root/compose.speech.yaml" "$@"; }
cleanup() {
  status=$?
  if [ "$status" -ne 0 ]; then compose logs >&2 || true; fi
  compose down >/dev/null || true
}
compose config --quiet
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
compose up -d --no-build
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
wait_ready "$(compose port speech 50073)"
media_endpoint=$(compose port media 50072)
wait_ready "$media_endpoint"
MEDIA_PIPELINE_TEST_TARGET="$media_endpoint" "$root/build/pipeline_tests" "$WHISPER_MODEL_PATH" "$WHISPER_VAD_MODEL_PATH" "$fixture"
for service in media speech; do
  container_id=$(compose ps -q "$service")
  hardening=$(docker inspect --format \
    'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}} writable={{range .Mounts}}{{.RW}}{{end}}' "$container_id")
  test "$hardening" = 'read_only=true user=65532:65532 mounts=1 writable=false'
  printf '%s\n' "$hardening"
done
