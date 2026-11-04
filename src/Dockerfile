FROM ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d AS compiler-base
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build gcc g++
WORKDIR /build
COPY CMakeLists.txt ./
COPY cmake/targets.cmake cmake/dependencies.cmake ./cmake/
ARG TDS_SANITIZERS=OFF

FROM compiler-base AS generator-build
RUN { g++ --version; sha256sum /usr/bin/g++ /usr/bin/ld; dpkg-query -W; } > /etc/tds-toolchain
COPY load ./load
RUN cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DTDS_GENERATOR_ONLY=ON -DTDS_SANITIZERS=${TDS_SANITIZERS} \
    && cmake --build out --target load_generator --parallel 2

FROM compiler-base AS build-base
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends make libssl-dev
RUN { g++ --version; sha256sum /usr/bin/g++ /usr/bin/ld; dpkg-query -W; } > /etc/tds-toolchain
COPY cmake/server-dependencies.cmake ./cmake/
COPY server ./server
COPY load ./load
COPY benchmarks/CMakeLists.txt ./benchmarks/CMakeLists.txt
COPY benchmarks/group_commit.cpp ./benchmarks/group_commit.cpp
COPY benchmarks/response_reuse.cpp ./benchmarks/response_reuse.cpp
COPY benchmarks/retry_receive.cpp ./benchmarks/retry_receive.cpp
COPY benchmarks/receive.cpp ./benchmarks/receive.cpp
COPY benchmarks/receive_backend.hpp ./benchmarks/receive_backend.hpp
COPY benchmarks/telemetry.cpp ./benchmarks/telemetry.cpp
COPY benchmarks/history.cpp ./benchmarks/history.cpp
COPY benchmarks/wal_advice.cpp ./benchmarks/wal_advice.cpp
COPY benchmarks/wal_vfs.cpp ./benchmarks/wal_vfs.cpp
COPY benchmarks/btree_post.cpp ./benchmarks/btree_post.cpp
COPY benchmarks/parser.cpp ./benchmarks/parser.cpp
COPY benchmarks/session.cpp ./benchmarks/session.cpp
COPY benchmarks/reply.cpp ./benchmarks/reply.cpp
ARG TDS_TELEMETRY=off

FROM build-base AS build
ARG TDS_BUILD_TARGET=server
RUN cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTDS_SANITIZERS=${TDS_SANITIZERS} -DTDS_TELEMETRY=${TDS_TELEMETRY} \
    && cmake --build out --target "${TDS_BUILD_TARGET}" --parallel 2

FROM build-base AS test-build
COPY tests/*.cpp tests/CMakeLists.txt ./tests/
RUN cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DTDS_SANITIZERS=${TDS_SANITIZERS} -DTDS_TELEMETRY=${TDS_TELEMETRY} \
    && cmake --build out --parallel 2

FROM ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d AS generator
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libstdc++6 \
    && useradd --uid 10001 --create-home benchmark
WORKDIR /app
COPY --from=generator-build /etc/tds-toolchain /etc/tds-toolchain
COPY --from=generator-build /build/out/load_generator ./load_generator
USER 10001:10001
ENTRYPOINT ["/app/load_generator"]

FROM ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d AS runtime-base
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libstdc++6 libssl3t64 curl jq openssl sqlite3 \
    && useradd --uid 10001 --create-home benchmark
WORKDIR /app
USER 10001:10001
ENV HOST=0.0.0.0 PORT=3000 SQLITE_PATH=/data/feed.db
ENTRYPOINT ["/app/server"]

FROM runtime-base AS runtime
COPY --from=build /etc/tds-toolchain /etc/tds-toolchain
COPY --from=build /build/out/server ./server

FROM runtime-base AS tested-runtime
COPY --from=test-build /etc/tds-toolchain /etc/tds-toolchain
COPY --from=test-build /build/out/server ./server

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
