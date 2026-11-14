#!/usr/bin/env bash
# Build the external generator in an isolated Ubuntu userspace; no host sysctls.
set -euo pipefail
submission_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
benchmark_src="$submission_root/deps/twelve-dollar-benchmark"
benchmark_revision=e31cd6285704b436422c2fc100d3beb10e558f6c
benchmark_image=ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
mkdir -p "$submission_root/deps"
if [[ ! -d "$benchmark_src/.git" ]]; then
  git clone --no-checkout https://github.com/mjc/twelve-dollar-benchmark.git "$benchmark_src"
  git -C "$benchmark_src" checkout --detach "$benchmark_revision"
fi
[[ $(git -C "$benchmark_src" rev-parse HEAD) == "$benchmark_revision" ]] || {
  echo "Existing benchmark checkout has a different revision; leaving it unchanged." >&2
  exit 1
}
[[ -z $(git -C "$benchmark_src" status --porcelain) ]] || {
  echo "Benchmark checkout has local changes; leaving them unchanged." >&2
  exit 1
}
docker run --rm --cpus 4 --memory 5g --memory-swap 5g \
  -v "$benchmark_src:/src:ro" \
  -v "$submission_root/deps/mjc-build:/build" \
  -v "$submission_root/deps/mjc-cargo:/cargo" \
  -v "$submission_root/deps/mjc-rustup:/rustup" \
  -e CARGO_HOME=/cargo -e RUSTUP_HOME=/rustup \
  "$benchmark_image" bash -lc '
    set -euo pipefail
    apt-get update
    apt-get install -y --no-install-recommends build-essential curl ca-certificates xz-utils pkg-config
    if [[ ! -x /cargo/bin/rustup ]]; then
      curl --proto =https --tlsv1.2 -sSf https://sh.rustup.rs |
        sh -s -- -y --profile minimal --default-toolchain none --no-modify-path
    fi
    /cargo/bin/rustup toolchain install 1.99.1 --profile minimal
    /cargo/bin/rustc +1.99.1 --version
    /cargo/bin/cargo +1.99.1 build --locked --release --manifest-path /src/Cargo.toml --target-dir /build -j 4
    /cargo/bin/cargo +1.99.1 test --locked --release --manifest-path /src/Cargo.toml --target-dir /build -j 4
    sha256sum /build/release/twelve-dollar-benchmark
  '
