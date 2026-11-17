#!/usr/bin/env bash
# Builds twelve-loop from this submission: fetches SQLite 3.54.1's autoconf archive, checks its SHA-256,
# publishes its sqlite3.c and sqlite3.h into vendor/, then runs
# cargo build --release --locked with this script's arguments appended unchanged.
#
# Usage: bash build.sh [cargo build arguments...], as the normal build user after install.sh.
# Environment: TWELVE_BUILD (unset or fast builds the fast build, safe the safe build); RUSTFLAGS (unset
# becomes -C target-cpu=native; a set value, empty included, is kept, and both builds append
# -C target-feature=-avx512f; the safe build then appends --cfg twelve_safe -C force-frame-pointers=yes; a
# value naming a twelve_ cfg flag is refused);
# TWELVE_C_TARGET (a GCC -march token, default native); CC (one compiler executable, default /usr/bin/gcc).
# Inherited CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS, _LTO and _CODEGEN_UNITS are cleared; only the safe build
# sets them, to true, false and 16.
# Needs: Bash 5.2+, curl, sha256sum, tar, mktemp, mkdir, mv, rm, dirname, env, GCC and the cargo proxy.
# Output: no stdout data of its own; native tool output stays as each tool writes it.
# Exit: 0 after the release build; 1 after a configuration refusal ("build.sh: invalid ..."), a filesystem refusal or a
# cleanup-only failure; otherwise the failed command's status after
# "build.sh: failed <stage> (status <status>)"; 130 on INT and 143 on TERM.
set -euo pipefail
# Bracket ranges in patterns follow ASCII order whatever the locale.
shopt -s globasciiranges

readonly sqlite_url=https://www.sqlite.org/2026/sqlite-autoconf-3540100.tar.gz
readonly sqlite_sha256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e
readonly sqlite_member_dir=sqlite-autoconf-3540100

tmp_dir=''

# Prints the stage summary line and returns the failed command's status.
fail() {
    printf 'build.sh: failed %s (status %d)\n' "$1" "$2" >&2
    return "$2"
}

# EXIT handler: runs after the last foreground child has finished, removes only this invocation's private
# tree and keeps the original status; a cleanup-only failure turns success into status 1.
cleanup_temp() {
    local status=$?
    local removed=0
    if [[ -n $tmp_dir ]]; then
        rm -rf -- "$tmp_dir" || removed=$?
    fi
    if ((removed != 0)); then
        printf 'build.sh: failed cleanup (status %d)\n' "$removed" >&2
        if ((status == 0)); then
            status=1
        fi
    fi
    exit "$status"
}

trap cleanup_temp EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

: "${TWELVE_BUILD=fast}"
if [[ $TWELVE_BUILD != fast && $TWELVE_BUILD != safe ]]; then
    printf 'build.sh: invalid TWELVE_BUILD (fast or safe)\n' >&2
    exit 1
fi
if [[ -z ${RUSTFLAGS+set} ]]; then
    RUSTFLAGS='-C target-cpu=native'
fi
if [[ $RUSTFLAGS == *twelve_safe* || $RUSTFLAGS == *twelve_small_limits* || $RUSTFLAGS == *twelve_fault_inject* ]]; then
    printf 'build.sh: invalid RUSTFLAGS\n' >&2
    exit 1
fi
# No 512-bit instructions in either build, whatever -C target-cpu names: a Cascade Lake core lowers its clock
# while it runs them and for about 2 ms after. build.rs passes the C unit's equal, -mno-avx512f.
RUSTFLAGS="$RUSTFLAGS -C target-feature=-avx512f"
: "${TWELVE_C_TARGET=native}"
: "${CC=/usr/bin/gcc}"

if [[ -z $TWELVE_C_TARGET || $TWELVE_C_TARGET == *[!A-Za-z0-9_-]* ]]; then
    printf 'build.sh: invalid TWELVE_C_TARGET\n' >&2
    exit 1
fi
if [[ -z $CC ]]; then
    printf 'build.sh: invalid CC\n' >&2
    exit 1
fi

# The safe build follows every rule the fast build's unsafe paths skip (Cargo.toml's release profile):
# Cargo's documented environment overrides of the release profile, never -C overflow-checks in RUSTFLAGS.
unset CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS CARGO_PROFILE_RELEASE_LTO CARGO_PROFILE_RELEASE_CODEGEN_UNITS
if [[ $TWELVE_BUILD == safe ]]; then
    RUSTFLAGS="$RUSTFLAGS --cfg twelve_safe -C force-frame-pointers=yes"
    export CARGO_PROFILE_RELEASE_OVERFLOW_CHECKS=true
    export CARGO_PROFILE_RELEASE_LTO=false
    export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16
fi
export RUSTFLAGS TWELVE_C_TARGET CC
readonly TWELVE_BUILD RUSTFLAGS TWELVE_C_TARGET CC

parent=$(dirname -- "${BASH_SOURCE[0]}") || fail locate "$?" || exit
root=$(CDPATH='' cd -- "$parent" && pwd -P) || fail locate "$?" || exit
readonly root
cd -- "$root" || fail directory "$?" || exit

mkdir -p -- "$root/target" || fail temporary "$?" || exit
# The subshell scopes umask 077 to the private directory; target/ keeps the default mode.
tmp_dir=$(umask 077 && mktemp -d "$root/target/build-sh.XXXXXXXXXX") || fail temporary "$?" || exit

# A stalled endpoint ends the download: at most 30 s to connect, and a transfer slower than 1 byte/s
# for 60 s stops; a slow download that still moves has no total cap.
curl --fail --silent --show-error --location --connect-timeout 30 --speed-limit 1 --speed-time 60 \
    --output "$tmp_dir/sqlite.tar.gz" "$sqlite_url" ||
    fail download "$?" || exit

# Checked from inside the private directory, so the record names only the fixed file name.
(
    cd -- "$tmp_dir" || exit
    printf '%s  sqlite.tar.gz\n' "$sqlite_sha256" | sha256sum --check -
) || fail checksum "$?" || exit

tar -xzf "$tmp_dir/sqlite.tar.gz" -C "$tmp_dir" --strip-components=1 \
    "$sqlite_member_dir/sqlite3.c" "$sqlite_member_dir/sqlite3.h" || fail extract "$?" || exit

if [[ -L $root/vendor ]] || { [[ -e $root/vendor ]] && [[ ! -d $root/vendor ]]; }; then
    fail vendor-directory 1 || exit
fi
mkdir -p -- "$root/vendor" || fail vendor-directory "$?" || exit
# rename(2) replaces each destination name whole, a symbolic link included, and never writes through it.
mv -f -T -- "$tmp_dir/sqlite3.c" "$root/vendor/sqlite3.c" || fail vendor-publish "$?" || exit
mv -f -T -- "$tmp_dir/sqlite3.h" "$root/vendor/sqlite3.h" || fail vendor-publish "$?" || exit

cargo build --release --locked "$@" || fail cargo "$?" || exit
