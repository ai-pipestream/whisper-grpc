#!/bin/sh
# Developer test driver only; no script is invoked by the media server.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
image=${1:-local/whisper-grpc:transport}
container_id=$(docker run -d --rm --read-only --cap-drop ALL \
  --security-opt no-new-privileges:true -p 127.0.0.1::50072 "$image")
trap 'docker stop "$container_id" >/dev/null' EXIT HUP INT TERM
endpoint=$(docker port "$container_id" 50072/tcp)
MEDIA_TEST_TARGET="$endpoint" "$root/build/media_tests"
# gRPC's built-in health service is registered but its descriptor need not be
# present in the application's generated-protobuf reflection pool.
health=$(grpcurl -plaintext -format text -import-path "$root/tests/proto" -proto health.proto \
  "$endpoint" grpc.health.v1.Health/Check)
case "$health" in
  *"status: SERVING"*) ;;
  *) printf 'Unexpected health response: %s\n' "$health" >&2; exit 1 ;;
esac
printf '%s\n' "$health"
hardening=$(docker inspect --format \
  'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}}' \
  "$container_id")
test "$hardening" = 'read_only=true user=65532:65532 mounts=0'
printf '%s\n' "$hardening"
