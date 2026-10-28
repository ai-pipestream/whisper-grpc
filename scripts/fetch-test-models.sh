#!/bin/sh
# Explicit qualification setup only; the services never download weights.
set -eu
test "$#" -eq 1 || { echo "Usage: $0 ASSET_DIRECTORY" >&2; exit 2; }
mkdir -p "$1"
cd "$1"
fetch() {
  name=$1
  digest=$2
  url=$3
  if [ ! -f "$name" ]; then
    temp=$(mktemp "./$name.XXXXXX")
    trap 'rm -f "$temp"' EXIT
    curl --fail --location --retry 5 --connect-timeout 15 --max-time 600 \
      --proto '=https' --proto-redir '=https' -o "$temp" "$url"
    printf '%s  %s\n' "$digest" "$temp" | sha256sum -c -
    mv "$temp" "$name"
    trap - EXIT
  fi
  printf '%s  %s\n' "$digest" "$name" | sha256sum -c -
}
revision=5359861c739e955e79d9a303bcbc70fb988958b1
fetch ggml-tiny.en.bin 921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f \
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/$revision/ggml-tiny.en.bin"
fetch ggml-base.bin 60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe \
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/$revision/ggml-base.bin"
fetch ggml-silero-v5.1.2.bin 29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf \
  https://huggingface.co/ggml-org/whisper-vad/resolve/9ffd54a1e1ee413ddf265af9913beaf518d1639b/ggml-silero-v5.1.2.bin
