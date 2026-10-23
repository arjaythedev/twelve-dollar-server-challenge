#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
source ./sqlite-build.sh
fetch() {
  [[ -f $2 ]] || curl -fsSL --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | sha256sum -c -
}
fetch https://mirrors.kernel.org/gnu/libmicrohttpd/libmicrohttpd-1.0.2.tar.gz \
  .deps/microhttpd.tar.gz df324fcd0834175dab07483133902d9774a605bfa298025f69883288fd20a8c7
fetch https://raw.githubusercontent.com/facil-io/cstl/24a57015d0989c64d5cc08ea9d24bd6cf97848bb/fio-stl.h \
  .deps/fio-stl.h fa05bdd4c193cf77cc1791f4e5a0c356c56e7abba38af284786231f0d0082154
mhd=.deps/libmicrohttpd-1.0.2
[[ -f $mhd/configure ]] || tar -xzf .deps/microhttpd.tar.gz -C .deps
if [[ ! -f $mhd/src/microhttpd/.libs/libmicrohttpd.a ]]; then
  (cd "$mhd"
    ./configure --disable-shared --enable-static --disable-https --disable-doc --disable-examples --disable-curl --disable-messages
    make -j2)
fi
cc -O3 -DNDEBUG -I.deps -I"$sqlite" -I"$mhd/src/include" -c bridge.c -o .deps/bridge.o
gfortran -O3 -flto -ffree-line-length-none -J.deps -I.deps \
  bindings.f90 unicode.f90 server.f90 .deps/bridge.o .deps/libsqlite3.a \
  "$mhd/src/microhttpd/.libs/libmicrohttpd.a" -lm -ldl -lpthread -o bin/server
