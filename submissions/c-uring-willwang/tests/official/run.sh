#!/bin/sh
set -eu
if [ "$#" -lt 2 ]; then
    echo 'Usage: run.sh WORK_DIR SERVER [TAG] --users COUNT [OPTIONS]' >&2
    exit 2
fi
scripts=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
work=$(CDPATH='' cd -- "$1" && pwd)
shift
original_net=$(readlink /proc/self/ns/net)
exec bwrap --unshare-user --unshare-net --ro-bind / / --bind "$work" "$work" \
    --proc /proc --dev /dev --die-with-parent --setenv ORIGINAL_NET_NS "$original_net" \
    python3 "$scripts/run.py" --work "$work" "$@"
