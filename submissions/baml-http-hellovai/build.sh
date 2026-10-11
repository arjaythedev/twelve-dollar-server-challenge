#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
BAML_REV=e7eb62db5d430e89806cc01e467d1726dd279eb9
mkdir -p .build bin
if [ ! -d .build/baml/.git ]; then
  git init .build/baml
  git -C .build/baml remote add origin https://github.com/BoundaryML/baml.git
fi
git -C .build/baml fetch --depth 1 origin "$BAML_REV"
git -C .build/baml checkout --detach "$BAML_REV"
export RUSTUP_HOME=/opt/baml-rust/rustup
export PATH="/opt/baml-rust/cargo/bin:$PATH"
# Serial compilation and no LTO keep the build usable on the 2 GB benchmark VM.
export CARGO_BUILD_JOBS=1 CARGO_PROFILE_RELEASE_LTO=false CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16
export CARGO_PROFILE_RELEASE_DEBUG=0
(
  cd .build/baml/baml_language
  cargo +1.98.0 build --locked --release -p baml_cli -p baml_pack_host
)
export BAML_TELEMETRY=off BAML_CLI_ALLOW_DIRECT=1
.build/baml/baml_language/target/release/baml-cli --agent-skill-check off pack main -o bin/server
