#!/bin/sh
# Build/test tooling only. No emulation, model downloads, or request-path scripts.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
case $(uname -m) in
  x86_64) arch=amd64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) echo 'Unsupported native CI architecture' >&2; exit 1 ;;
esac
if [ "$arch" != "${EXPECTED_ARCH:-$arch}" ]; then
  echo "Host architecture $arch does not match EXPECTED_ARCH=$EXPECTED_ARCH" >&2
  exit 1
fi
case $(docker info --format '{{.Architecture}}') in
  x86_64|amd64) docker_arch=amd64 ;;
  aarch64|arm64) docker_arch=arm64 ;;
  *) echo 'Unsupported Docker architecture' >&2; exit 1 ;;
esac
if [ "$arch" != "$docker_arch" ]; then
  echo "Docker architecture $docker_arch differs from host $arch; native testing requires a matching daemon" >&2
  exit 1
fi
builder="local/whisper-grpc:ci-builder-$arch"
runtime="local/whisper-grpc:ci-runtime-$arch"
# The build stage runs every model-independent CTest executable.
docker build --target build -t "$builder" .
docker build -t "$runtime" .
test "$(docker image inspect --format '{{.Architecture}}' "$runtime")" = "$arch"
container_id=$(docker run -d --rm --read-only --cap-drop ALL \
  --security-opt no-new-privileges:true "$runtime")
cleanup() {
  docker logs "$container_id" || true
  docker stop "$container_id" >/dev/null || true
}
trap cleanup EXIT HUP INT TERM
# The builder supplies a matching native test client and shared-library closure.
# Sharing only the network namespace keeps the runtime filesystem independent.
docker run --rm --read-only --cap-drop ALL --security-opt no-new-privileges:true \
  --network "container:$container_id" -e MEDIA_TEST_TARGET=127.0.0.1:50072 \
  "$builder" /src/build/media_tests
hardening=$(docker inspect --format \
  'read_only={{.HostConfig.ReadonlyRootfs}} user={{.Config.User}} mounts={{len .Mounts}}' \
  "$container_id")
test "$hardening" = 'read_only=true user=65532:65532 mounts=0'
printf 'Native %s runtime verified: %s\n' "$arch" "$hardening"
