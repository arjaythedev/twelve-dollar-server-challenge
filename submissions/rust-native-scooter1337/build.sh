#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ -x /opt/twelve-rust/cargo/bin/cargo ]]; then
  export RUSTUP_HOME=/opt/twelve-rust/rustup
  export PATH="/opt/twelve-rust/cargo/bin:$PATH"
else
  export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
fi
export RUSTUP_TOOLCHAIN=1.94.0
mkdir -p vendor
if [[ ! -f vendor/sqlite3.c ]]; then
  curl -fsSL --retry 3 https://sqlite.org/2026/sqlite-amalgamation-3540100.zip -o vendor/sqlite.zip
  printf '%s  %s\n' 0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9 vendor/sqlite.zip | shasum -a 256 -c -
  python3 - <<'PY'
import zipfile
from pathlib import Path
with zipfile.ZipFile('vendor/sqlite.zip') as z:
    for name in ('sqlite3.c', 'sqlite3.h'):
        Path('vendor',name).write_bytes(z.read('sqlite-amalgamation-3540100/'+name))
Path('vendor/sqlite.zip').unlink()
PY
fi
export RUSTFLAGS="-C target-cpu=native ${RUSTFLAGS:-}"
cargo build --release --locked "$@"
