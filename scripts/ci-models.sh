#!/bin/sh
# Run already-built native test executables with operator-supplied test assets.
set -eu
test "$#" -eq 2 || { echo "Usage: $0 BUILDER_IMAGE ASSET_DIRECTORY" >&2; exit 2; }
assets=$(realpath "$2")
cd "$assets"
sha256sum -c - <<'CHECKSUMS'
921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f  ggml-tiny.en.bin
60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe  ggml-base.bin
29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf  ggml-silero-v5.1.2.bin
CHECKSUMS
docker run --rm --read-only --cap-drop ALL --security-opt no-new-privileges:true \
  --mount "type=bind,src=$assets,dst=/models,readonly" "$1" /bin/sh -eu -c '
  cd /src
  fixture=/src/build/_deps/whisper-src/samples/jfk.wav
  printf "%s  %s\n" 59dfb9a4acb36fe2a2affc14bacbee2920ff435cb13cc314a08c13f66ba7860e "$fixture" | sha256sum -c -
  run() { exe=$1; shift; printf "Running %s\n" "$exe"; timeout 420 "build/$exe" "$@"; }
  run vad_tests /models/ggml-silero-v5.1.2.bin "$fixture"
  run speech_window_probe /models/ggml-tiny.en.bin "$fixture"
  run speech_tests /models/ggml-tiny.en.bin "$fixture"
  run pipeline_cancellation_tests /models/ggml-tiny.en.bin
  run pipeline_tests /models/ggml-tiny.en.bin /models/ggml-silero-v5.1.2.bin "$fixture"
  run translation_tests /models/ggml-base.bin tests/fixtures/minds14-es-0.wav minds14
  printf "All six model-backed qualification executables passed\n"
  '
