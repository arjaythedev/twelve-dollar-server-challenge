#!/usr/bin/env bash
# Root install for the twelve-loop build: Ubuntu's build-essential, curl and ca-certificates, then the
# pinned Rust 1.94.0 toolchain (minimal profile, rustfmt and clippy) under /opt/rust, with proxies in
# /usr/local/bin that a normal build user runs without access to root's home.
#
# Usage: bash install.sh, as root, on Ubuntu 24.04 with Bash 5.2+. Takes no arguments.
# Needs: apt-get, uname, mktemp, mkdir, chmod, printf, sha256sum, mv, rm.
# Reads: TMPDIR for the private download directory (the platform's /tmp when unset).
# Output: no stdout data; native package and installer progress, and this script's own lines, go to stderr.
# Exit: 0 after packages, toolchain and proxies are installed; 1 for a refusal or a cleanup-only failure;
# a failed command's own status otherwise, after "install.sh: failed <stage> (status <status>)";
# 130 on INT and 143 on TERM.
set -u
set -o pipefail

readonly rustup_version=1.28.2
readonly toolchain=1.94.0
readonly rust_root=/opt/rust
readonly proxy_dir=/usr/local/bin

tmp_dir=''
proxy_tmp=''

# Prints the stage summary line and returns the failed command's status.
fail() {
    printf 'install.sh: failed %s (status %d)\n' "$1" "$2" >&2
    return "$2"
}

# EXIT handler: removes only this invocation's temporaries and keeps the original status; a cleanup-only
# failure turns success into status 1.
cleanup_temp() {
    local status=$?
    local removed=0
    if [[ -n $proxy_tmp ]]; then
        rm -f -- "$proxy_tmp" || removed=$?
    fi
    if [[ -n $tmp_dir ]]; then
        rm -rf -- "$tmp_dir" || removed=$?
    fi
    if ((removed != 0)); then
        printf 'install.sh: failed cleanup (status %d)\n' "$removed" >&2
        if ((status == 0)); then
            status=1
        fi
    fi
    exit "$status"
}

trap cleanup_temp EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if ((EUID != 0)); then
    printf 'install.sh: run as root\n' >&2
    exit 1
fi

machine=$(uname -m) || fail machine "$?" || exit
case $machine in
x86_64)
    triple=x86_64-unknown-linux-gnu
    installer_sha256=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c
    ;;
aarch64)
    triple=aarch64-unknown-linux-gnu
    installer_sha256=e3853c5a252fca15252d07cb23a1bdd9377a8c6f3efa01531109281ae47f841c
    ;;
*)
    printf 'install.sh: unsupported machine %q\n' "$machine" >&2
    exit 1
    ;;
esac
readonly triple installer_sha256

DEBIAN_FRONTEND=noninteractive apt-get update >&2 || fail packages-update "$?" || exit
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential curl ca-certificates >&2 || fail packages-install "$?" || exit

# The subshell scopes umask 077 to the private directory.
tmp_dir=$(umask 077 && mktemp -d) || fail temporary-directory "$?" || exit

# The basename stays rustup-init: rustup picks its role from the name it runs under. A stalled
# endpoint ends the download: at most 30 s to connect, and a transfer slower than 1 byte/s for 60 s
# stops; a slow download that still moves has no total cap.
curl --fail --silent --show-error --location --connect-timeout 30 --speed-limit 1 --speed-time 60 \
    --output "$tmp_dir/rustup-init" \
    "https://static.rust-lang.org/rustup/archive/$rustup_version/$triple/rustup-init" >&2 ||
    fail download "$?" || exit

# Checked from inside the directory, so the record names only the fixed basename.
(
    cd -- "$tmp_dir" || exit
    printf '%s  rustup-init\n' "$installer_sha256" | sha256sum --check - >&2
) || fail checksum "$?" || exit

chmod 700 -- "$tmp_dir/rustup-init" || fail installer-mode "$?" || exit

mkdir -p -- "$rust_root" || fail toolchain-directory "$?" || exit

RUSTUP_HOME="$rust_root/rustup" CARGO_HOME="$rust_root/cargo" \
    RUSTUP_DIST_SERVER=https://static.rust-lang.org RUSTUP_UPDATE_ROOT=https://static.rust-lang.org/rustup \
    "$tmp_dir/rustup-init" -y --profile minimal --default-toolchain "$toolchain" --default-host "$triple" \
    --component rustfmt --component clippy --no-modify-path >&2 ||
    fail toolchain-install "$?" || exit

chmod -R a+rX,go-w -- "$rust_root" || fail toolchain-permissions "$?" || exit

for tool in cargo rustc rustdoc rustfmt cargo-fmt cargo-clippy clippy-driver rustup; do
    proxy_tmp=$(mktemp "$proxy_dir/.$tool.XXXXXXXXXX") || fail proxy-temporary "$?" || exit
    # The generated proxy expands "$@" itself; this printf only fills in the two fixed paths.
    # shellcheck disable=SC2016
    printf '#!/bin/sh\nRUSTUP_HOME="%s"\nexport RUSTUP_HOME\nexec "%s" "$@"\n' \
        "$rust_root/rustup" "$rust_root/cargo/bin/$tool" >"$proxy_tmp" || fail proxy-write "$?" || exit
    chmod 755 -- "$proxy_tmp" || fail proxy-mode "$?" || exit
    mv -f -T -- "$proxy_tmp" "$proxy_dir/$tool" || fail proxy-publish "$?" || exit
    proxy_tmp=''
done
