#!/usr/bin/env bash
set -euo pipefail
if [[ $(uname -s) == Linux ]]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends build-essential ca-certificates curl unzip python3
else
  # macOS: use the existing Xcode command-line tools.
  for tool in cc curl unzip shasum python3; do command -v "$tool" >/dev/null; done
fi
