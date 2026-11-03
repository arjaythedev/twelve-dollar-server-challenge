#!/usr/bin/env bash
set -euo pipefail
if [[ $(uname -s) == Linux ]]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends build-essential clang ca-certificates curl unzip
else
  # macOS: use the existing Xcode command-line tools.
  for tool in cc clang++ ar curl tar unzip shasum; do command -v "$tool" >/dev/null; done
fi
