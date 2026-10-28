#!/bin/sh
# Called only after both native qualification jobs have succeeded.
set -eu
test "$#" -eq 3 || { echo "Usage: $0 COMMIT AMD64_DIGEST_FILE ARM64_DIGEST_FILE" >&2; exit 2; }
commit=$1
test "${#commit}" -eq 40
printf '%s\n' "$commit" | grep -Eq '^[0-9a-f]{40}$'
repo=pipestreamai/whisper-grpc
amd=$(cat "$2")
arm=$(cat "$3")
for ref in "$amd" "$arm"; do
  test "${#ref}" -eq "$((${#repo}+72))"
  printf '%s\n' "$ref" | grep -Eq '^pipestreamai/whisper-grpc@sha256:[0-9a-f]{64}$'
done
test "$amd" != "$arm"
# Pulling an ARM image on this host inspects it only; no ARM code is executed.
docker pull --platform linux/amd64 "$amd"
test "$(docker image inspect --format '{{.Os}}/{{.Architecture}}' "$amd")" = linux/amd64
docker pull --platform linux/arm64 "$arm"
test "$(docker image inspect --format '{{.Os}}/{{.Architecture}}' "$arm")" = linux/arm64
docker buildx imagetools create -t "$repo:sha-$commit" -t "$repo:latest" "$amd" "$arm"
docker buildx imagetools inspect --raw "$repo:sha-$commit" |
  jq -e '[.manifests[].platform | select(.os == "linux") | .architecture] | sort == ["amd64", "arm64"]'
