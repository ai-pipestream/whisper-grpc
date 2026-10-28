FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make pkg-config libgrpc++-dev libprotobuf-dev \
    protobuf-compiler protobuf-compiler-grpc libssl-dev libcurl4-openssl-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY proto ./proto
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j2 && ctest --test-dir build --output-on-failure

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libgrpc++1.51t64 libprotobuf32t64 libssl3t64 libgomp1 libcurl4t64 \
    libavformat61 libavcodec61 libavutil59 libswresample5 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/whisper-media-server /usr/local/bin/whisper-media-server
COPY --from=build /src/build/whisper-speech-worker /usr/local/bin/whisper-speech-worker
COPY --from=build /src/build/bin/lib*.so* /usr/local/lib/whisper/
ENV LD_LIBRARY_PATH=/usr/local/lib/whisper
USER 65532:65532
ENV WHISPER_MEDIA_LISTEN=0.0.0.0:50072
EXPOSE 50072
ENTRYPOINT ["/usr/local/bin/whisper-media-server"]
