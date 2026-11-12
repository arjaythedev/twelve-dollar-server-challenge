#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
    echo 'Usage: build-client.sh WORK_DIR' >&2
    exit 2
fi
scripts=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
work=$(CDPATH='' cd -- "$1" && pwd)
go_bin=${GO_BIN:-go}
cd "$scripts/mload"
printf '%s\n' 'a223cbb43509b0bdf51c09d983d79742f80cf31f7a47433718fb5f867822d8da  main.go' \
    '0240e0d025d9da1f15584494f5c210c722ca20555e74ccd7c0f582561628af8a  go.mod' | sha256sum -c -
export GOWORK=off GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off CGO_ENABLED=0 GOFLAGS=''
temp=$(mktemp -d "$work/.build.XXXXXX")
trap 'rm -f -- "$temp/mload" "$temp/mload.build.txt" "$temp/mload.sha256"; rmdir -- "$temp"' EXIT
"$go_bin" build -trimpath -buildvcs=false -o "$temp/mload" .
"$go_bin" version -m "$temp/mload" > "$temp/mload.build.txt"
sha256sum "$temp/mload" | cut -d' ' -f1 > "$temp/mload.sha256"
if [ "$("$go_bin" version)" = 'go version go1.27.3 linux/amd64' ]; then
    printf '%s  %s\n' 48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6 "$temp/mload" | sha256sum -c -
fi
mv -- "$temp/mload" "$temp/mload.sha256" "$temp/mload.build.txt" "$work/"
