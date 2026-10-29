#!/bin/sh
# Deployment qualification only; services never invoke this script.
set -eu
test "$#" -eq 4 || { echo "Usage: $0 RUNTIME_IMAGE BUILDER_IMAGE ASSETS whisper|parakeet" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runtime=$1
builder=$2
assets=$(realpath "$3")
backend=$4
case $(uname -m) in
  x86_64) arch=amd64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) echo 'Unsupported native architecture' >&2; exit 1 ;;
esac
case $(docker info --format '{{.Architecture}}') in
  x86_64|amd64) daemon_arch=amd64 ;;
  aarch64|arm64) daemon_arch=arm64 ;;
  *) echo 'Unsupported Docker architecture' >&2; exit 1 ;;
esac
test "$daemon_arch" = "$arch"
test "$(docker image inspect --format '{{.Architecture}}' "$runtime")" = "$arch"
test "$(docker image inspect --format '{{.Architecture}}' "$builder")" = "$arch"
case "$backend" in
  whisper)
    model=ggml-tiny.en.bin; model_id=test-tiny.en
    checksum=921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f ;;
  parakeet)
    model=ggml-parakeet-tdt-0.6b-v3-q8_0.bin; model_id=parakeet-test
    checksum=4d64e9e96c2792186d072fde0034df0ad670cf680a2f53069052ead827fd600e ;;
  *) echo 'Unsupported speech backend' >&2; exit 2 ;;
esac
(cd "$assets" && printf '%s  %s\n' "$checksum" "$model" \
  29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf ggml-silero-v5.1.2.bin | sha256sum -c -)
export WHISPER_IMAGE="$runtime" WHISPER_BACKEND="$backend" WHISPER_MODEL_ID="$model_id"
export WHISPER_MODEL_PATH="$assets/$model" WHISPER_VAD_MODEL_PATH="$assets/ggml-silero-v5.1.2.bin"
export WHISPER_MEDIA_PORT=0 WHISPER_WORKER_PORT=0 WHISPER_THREADS=4
# The fixture runs in media's isolated network namespace, never on a host port.
export WHISPER_HTTP_ORIGINS=http://127.0.0.1:18081 WHISPER_HTTP_ALLOW_PRIVATE=1
project=whisper-compose-$backend-$$
compose() { docker compose -p "$project" -f "$root/compose.speech.yaml" "$@"; }
cleanup() {
  result=$?
  if [ "$result" -ne 0 ]; then compose logs >&2 || true; fi
  compose down >/dev/null || true
}
compose config --quiet
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
compose up -d --no-build
for service in speech media; do
  container=$(compose ps -q "$service")
  attempt=0
  until docker logs "$container" 2>&1 | grep -q 'listening on'; do
    test "$(docker inspect --format '{{.State.Running}}' "$container")" = true
    attempt=$((attempt+1)); test "$attempt" -le 60
    sleep 1
  done
done
media_container=$(compose ps -q media)
docker run --rm --read-only --cap-drop ALL --security-opt no-new-privileges:true \
  --network "container:$media_container" --mount "type=bind,src=$assets,dst=/models,readonly" \
  -e MEDIA_PIPELINE_TEST_TARGET=127.0.0.1:50072 \
  -e MEDIA_PIPELINE_HTTP_TARGET=127.0.0.1:50072 -e MEDIA_PIPELINE_HTTP_PORT=18081 \
  "$builder" timeout 600 /src/build/pipeline_tests \
  "/models/$model" /models/ggml-silero-v5.1.2.bin \
  /src/build/_deps/whisper-src/samples/jfk.wav "$backend"
for service in media speech; do
  container=$(compose ps -q "$service")
  hardening=$(docker inspect --format 'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}} writable={{range .Mounts}}{{.RW}}{{end}}' "$container")
  test "$hardening" = 'read_only=true user=65532:65532 mounts=1 writable=false'
  printf 'Compose %s %s verified: %s\n' "$backend" "$service" "$hardening"
done
