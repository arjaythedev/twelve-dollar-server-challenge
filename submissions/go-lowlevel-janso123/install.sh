#!/usr/bin/env bash
# Pinned cgo/PGO toolchain for Ubuntu 24.04 Linux x86_64. Run as root.
set -euo pipefail

die() { echo "install: $*" >&2; exit 1; }
[[ $(id -u) == 0 ]] || die "run this script as root"
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die "requires Linux x86_64"
[[ -r /etc/os-release ]] || die "requires Ubuntu 24.04"
# shellcheck disable=SC1091
source /etc/os-release
[[ ${ID:-} == ubuntu && ${VERSION_ID:-} == 24.04 ]] || die "requires Ubuntu 24.04"

readonly LLVM_VERSION=23.1.3 GO_VERSION=1.27.2
readonly LLVM_SHA256=14d2f701eb68fb799001bdea6231048f3990690fa2f406d563555ff8f744daba
readonly GO_SHA256=ecbadb99091a3f46e31f5f934b068b1864eafa7995211b39eaddf76996045fe5
readonly LLVM_DEST=/opt/llvm-23.1.3 GO_DEST=/usr/local/go
readonly LLVM_ARCHIVE="LLVM-${LLVM_VERSION}-Linux-X64.tar.zst"
readonly GO_ARCHIVE="go${GO_VERSION}.linux-amd64.tar.gz"
readonly LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/${LLVM_ARCHIVE}"

work='' llvm_stage='' go_stage='' committed=0

# Staging lives on the destination filesystems, so promotion uses renames.
# Keep previous installs until BOTH replacements have succeeded.
rollback() {
    local stage=$1 dest=$2
    [[ -n $stage ]] || return 0
    if [[ -e $stage/previous || -L $stage/previous ]]; then
        rm -rf -- "$dest" && mv -T -- "$stage/previous" "$dest" || return 1
    elif [[ -f $stage/promoting && ! -e $stage/new ]]; then
        rm -rf -- "$dest" || return 1
    fi
}

cleanup() {
    local status=$? stage dest index
    trap - EXIT
    local stages=("$go_stage" "$llvm_stage")
    local destinations=("$GO_DEST" "$LLVM_DEST")
    for index in 0 1; do
        stage=${stages[$index]} dest=${destinations[$index]}
        [[ -n $stage ]] || continue
        if [[ $committed == 0 ]] && ! rollback "$stage" "$dest"; then
            echo "install: rollback failed; recover $dest from $stage/previous" >&2
            status=1
            continue  # Preserve the backup for manual recovery.
        fi
        rm -rf -- "$stage" || status=1
    done
    [[ -z $work ]] || rm -rf -- "$work" || status=1
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

export DEBIAN_FRONTEND=noninteractive
apt-get update
# System headers/linker inputs plus the shared-library dependencies of LLVM's
# official Linux binaries. No kernel, sysctl, or resource-limit changes.
apt-get install -y --no-install-recommends \
    python3 make build-essential binutils zstd curl ca-certificates \
    libtinfo6 libxml2 libzstd1 zlib1g libedit2 libffi8 libgcc-s1 libstdc++6 libatomic1

work=$(mktemp -d "${TMPDIR:-/tmp}/go-llvm-install.XXXXXX")
mkdir -p /opt /usr/local
llvm_stage=$(mktemp -d "/opt/.llvm-${LLVM_VERSION}.install.XXXXXX")
go_stage=$(mktemp -d "/usr/local/.go-${GO_VERSION}.install.XXXXXX")
mkdir "$llvm_stage/unpack" "$go_stage/unpack"

download() {
    local url=$1 archive=$2 checksum=$3
    curl --fail --show-error --silent --location --retry 5 --retry-delay 2 \
        --retry-all-errors --connect-timeout 30 -o "$work/$archive" "$url"
    printf '%s  %s\n' "$checksum" "$work/$archive" | sha256sum -c -
}
download "$LLVM_URL" "$LLVM_ARCHIVE" "$LLVM_SHA256"
download "https://go.dev/dl/$GO_ARCHIVE" "$GO_ARCHIVE" "$GO_SHA256"

# The official zstd archive uses a 1 GiB window; Ubuntu defaults to 128 MiB.
# Inspect the verified archive: release filenames do not promise a root name.
# Select exact members, avoiding static libraries, docs, and unrelated tools.
tar --use-compress-program='zstd --long=30' -tf "$work/$LLVM_ARCHIVE" > "$work/llvm.list"
python3 - "$work/llvm.list" "$work/llvm.members" "$work/llvm.root" <<'PY'
import fnmatch
from pathlib import Path, PurePosixPath
import sys

listing, selection, root_file = map(Path, sys.argv[1:])
members = []
roots = set()
for original in listing.read_text().splitlines():
    path = PurePosixPath(original)
    if path.is_absolute() or ".." in path.parts:
        sys.exit("install: unsafe LLVM archive path")
    if not path.parts:  # A harmless './' directory entry.
        continue
    roots.add(path.parts[0])
    members.append((original, path.parts[1:]))
if len(roots) != 1:
    sys.exit("install: expected exactly one LLVM archive root")

tools = {"clang", "clang-23", "clang++", "llvm-ar", "llvm-profdata", "ld.lld", "lld"}
selected = []
for original, parts in members:
    tool = len(parts) == 2 and parts[0] == "bin" and parts[1] in tools
    resources = parts[:2] == ("lib", "clang")
    # Some official builds use shared LLVM libraries; keep only these runtime
    # libraries when present, rather than unpacking the whole lib directory.
    runtime = len(parts) == 2 and parts[0] == "lib" and any(
        fnmatch.fnmatchcase(parts[1], pattern)
        for pattern in ("libLLVM*.so*", "libclang-cpp*.so*")
    )
    if tool or resources or runtime:
        selected.append(original)
if not selected:
    sys.exit("install: LLVM archive contains no toolchain members")
selection.write_text("\n".join(selected) + "\n")
root_file.write_text(roots.pop() + "\n")
PY
IFS= read -r llvm_root < "$work/llvm.root"
tar --use-compress-program='zstd --long=30' -xf "$work/$LLVM_ARCHIVE" -C "$llvm_stage/unpack" \
    --no-same-owner --no-same-permissions --no-recursion \
    --verbatim-files-from -T "$work/llvm.members"
mv -T -- "$llvm_stage/unpack/$llvm_root" "$llvm_stage/new"
tar -xzf "$work/$GO_ARCHIVE" -C "$go_stage/unpack" --no-same-owner --no-same-permissions
mv -T -- "$go_stage/unpack/go" "$go_stage/new"

# Execute every selected tool before touching the current installations.
verify_llvm_version() {
    "$@" | grep -E '(^|[^0-9.])23\.1\.3([^0-9.]|$)' > /dev/null \
        || die "staged LLVM version check failed: $*"
}
for tool in clang clang-23 clang++ llvm-ar llvm-profdata ld.lld; do
    verify_llvm_version "$llvm_stage/new/bin/$tool" --version
done
# lld itself is a generic driver; select the ELF flavor explicitly.
verify_llvm_version "$llvm_stage/new/bin/lld" -flavor gnu --version
resource_dir=$("$llvm_stage/new/bin/clang" -print-resource-dir)
resource_dir=$(cd -- "$resource_dir" && pwd -P) || die "staged Clang resources are missing"
resource_root=$(cd -- "$llvm_stage/new/lib/clang" && pwd -P) || die "staged Clang resources are missing"
[[ $resource_dir == "$resource_root/"* && -f $resource_dir/include/stddef.h ]] \
    || die "staged Clang resources are missing"
cat > "$work/smoke.c" <<'C'
#include <stddef.h>
#include <stdio.h>
int main(void) { return puts("LLVM toolchain verified") < 0; }
C
"$llvm_stage/new/bin/clang" -fuse-ld=lld --ld-path="$llvm_stage/new/bin/ld.lld" \
    "$work/smoke.c" -o "$work/smoke"
"$work/smoke"
GOROOT="$go_stage/new" GOTOOLCHAIN=local "$go_stage/new/bin/go" version \
    | grep -Fx "go version go${GO_VERSION} linux/amd64" \
    || die "staged Go version check failed"

promote() {
    local stage=$1 dest=$2
    if [[ -e $dest || -L $dest ]]; then
        mv -T -- "$dest" "$stage/previous"
    fi
    touch "$stage/promoting"
    mv -T -- "$stage/new" "$dest"
}
promote "$llvm_stage" "$LLVM_DEST"
promote "$go_stage" "$GO_DEST"
committed=1
printf 'Installed LLVM %s at %s and Go %s at %s\n' \
    "$LLVM_VERSION" "$LLVM_DEST" "$GO_VERSION" "$GO_DEST"
