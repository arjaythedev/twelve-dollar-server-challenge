FROM ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d AS build
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build make gcc g++ libssl-dev
WORKDIR /build
COPY CMakeLists.txt ./
COPY server ./server
COPY checks/transport.cpp ./checks/transport.cpp
COPY checks/checkpoint.cpp ./checks/checkpoint.cpp
COPY checks/group_commit.cpp ./checks/group_commit.cpp
COPY checks/post_writer.cpp ./checks/post_writer.cpp
COPY checks/btree.cpp ./checks/btree.cpp
COPY checks/sync.cpp ./checks/sync.cpp
COPY load ./load
COPY benchmarks/group_commit.cpp ./benchmarks/group_commit.cpp
ARG TDS_SANITIZERS=OFF
ARG TDS_DIAGNOSTICS=OFF
RUN cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DTDS_SANITIZERS=${TDS_SANITIZERS} -DTDS_DIAGNOSTICS=${TDS_DIAGNOSTICS} \
    && cmake --build out --parallel 2

FROM ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d AS runtime
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libstdc++6 libssl3t64 curl jq openssl sqlite3 \
    && useradd --uid 10001 --create-home benchmark
WORKDIR /app
COPY --from=build /build/out/server ./server
COPY --from=build /build/out/load_generator ./load_generator
USER 10001:10001
ENV HOST=0.0.0.0 PORT=3000 SQLITE_PATH=/data/feed.db
ENTRYPOINT ["/app/server"]

FROM runtime AS profiler
USER root
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends linux-tools-generic \
    && perf_path="$(find /usr/lib -path '*/linux-tools*/perf' -type f -print -quit)" \
    && test -n "$perf_path" \
    && ln -s "$perf_path" /usr/local/bin/perf
USER 10001:10001
ENTRYPOINT ["perf"]

FROM runtime AS final
