#!/usr/bin/env bash
# Builds zig-out/bin/server. Downloads anyzig as ./zig, which in turn fetches
# the actual zig version needed in build.zig.zon.
set -euo pipefail
cd "$(dirname "$0")"
anyzig_release=v2026_10_13
case "$(uname -m)" in
  x86_64)  arch=x86_64;  sha256=e4742a7ce7835828efb93d2cfa3c9d07fead33755a8bb7185d2159fda872018e ;;
  aarch64) arch=aarch64; sha256=72f90cc760169c7a958372f56866773b9cca23ae7938f12f740f042bb1264f20 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
if [ ! -x zig ]; then
  # Download and extract next to the final path, then rename: an interrupted
  # run never leaves a partial ./zig behind.
  tmp="$(mktemp -d zig.tmp.XXXXXX)"
  trap 'rm -rf "$tmp"' EXIT
  curl -fsSL -o "$tmp/anyzig.tar.gz" "https://github.com/marler8997/anyzig/releases/download/$anyzig_release/anyzig-$arch-linux.tar.gz"
  echo "$sha256  $tmp/anyzig.tar.gz" | sha256sum -c --quiet
  tar xzf "$tmp/anyzig.tar.gz" -C "$tmp" zig
  mv "$tmp/zig" zig
fi
./zig build
