#!/bin/sh
# pgo.sh SQLITE_DIR PLANS_DIR OUT.o CFLAGS... : compiles feedbt.c (SQLite plus the plans) into OUT.o.
# With GCC and no FEED_PGO=0 it is profile-guided: an instrumented build runs feedbt_train.c on a synthetic database, then
# feedbt.c is compiled again with that profile. The profile lives in a temporary directory and is deleted; nothing of it is
# committed or shipped. Any failure of the training steps falls back to the plain build, so the result is always correct.
set -u
sq=$1; plans=$2; out=$3; shift 3
plain() { cc "$@" -I"$sq" -I"$plans" -c "$plans/feedbt.c" -o "$out"; }
if [ "${FEED_PGO:-1}" = 0 ] || ! cc -v 2>&1 | grep -q '^gcc version'; then plain "$@"; exit $?; fi
t=$(mktemp -d) || { plain "$@"; exit $?; }
trap 'rm -rf "$t"' EXIT
if cc "$@" -fprofile-generate -fprofile-update=single -fprofile-dir="$t" -I"$sq" -I"$plans" -c "$plans/feedbt.c" -o "$t/gen.o" \
  && cc -O2 -I"$sq" -I"$plans" -c "$plans/feedbt_train.c" -o "$t/train.o" \
  && cc "$t/train.o" "$t/gen.o" -fprofile-generate -lm -lpthread -o "$t/train" \
  && (cd "$t" && ./train "$t/train.db" > /dev/null) \
  && cc "$@" -fprofile-use -fprofile-dir="$t" -fprofile-correction -Wno-missing-profile -I"$sq" -I"$plans" -c "$plans/feedbt.c" -o "$out"; then
  echo "pgo.sh: profile-guided build of feedbt.c" >&2
else
  echo "pgo.sh: profile-guided build failed, plain build" >&2
  plain "$@"
fi
