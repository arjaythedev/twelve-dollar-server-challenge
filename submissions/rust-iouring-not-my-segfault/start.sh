#!/usr/bin/env bash
# Runs the built server in the foreground: replaces this shell with target/release/twelve-loop from this
# script's folder, with no arguments. The server reads SQLITE_PATH, JWT_SECRET, HOST and PORT itself.
#
# Usage: bash start.sh. Arguments given to this script are not passed on. Needs Bash 5.2+ and dirname.
# Output: none of its own after a successful exec; the server owns every stream from then on.
# Exit: the server's own status after exec; before it, the failed command's status after
# "start.sh: cannot resolve script directory" or "start.sh: cannot execute target/release/twelve-loop (status <status>)".
set -u
set -o pipefail
# Only so that a failed exec returns here and reaches the summary line below.
shopt -s execfail

parent=$(dirname -- "${BASH_SOURCE[0]}") || {
    status=$?
    printf 'start.sh: cannot resolve script directory\n' >&2
    exit "$status"
}
# CDPATH is cleared so cd neither searches it nor prints the folder into the captured output.
folder=$(CDPATH='' cd -- "$parent" && pwd -P) || {
    status=$?
    printf 'start.sh: cannot resolve script directory\n' >&2
    exit "$status"
}

exec "$folder/target/release/twelve-loop"
status=$?
printf 'start.sh: cannot execute target/release/twelve-loop (status %d)\n' "$status" >&2
exit "$status"
