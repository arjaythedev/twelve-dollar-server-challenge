#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .deps bin
checksum=(shasum -a 256)
if command -v sha256sum >/dev/null; then checksum=(sha256sum); fi
fetch() {
  [[ -f $2 ]] || curl -fsSL --retry 3 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | "${checksum[@]}" -c -
}
fetch https://codeload.github.com/uNetworking/uWebSockets/tar.gz/e2ec2d7af011203420e782fb649d27934fc2bdea \
  .deps/uwebsockets.tar.gz 8086a1280671fa39cffbe4121860516260106edf2a31d55eb3f12009f216d181
fetch https://codeload.github.com/uNetworking/uSockets/tar.gz/86097c490263ab662d62e8e7b541390bdec7d149 \
  .deps/usockets.tar.gz 0d341b94157720d9081d47348a8cba87ae350b6607c2f7d2ccf102353cbda553
fetch https://raw.githubusercontent.com/facil-io/cstl/24a57015d0989c64d5cc08ea9d24bd6cf97848bb/fio-stl.h \
  .deps/fio-stl.h fa05bdd4c193cf77cc1791f4e5a0c356c56e7abba38af284786231f0d0082154
fetch https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip \
  .deps/sqlite.zip 1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
for name in uwebsockets usockets; do
  if [[ ! -f .deps/$name/LICENSE ]]; then
    mkdir -p ".deps/$name"
    tar -xzf ".deps/$name.tar.gz" --strip-components=1 -C ".deps/$name"
  fi
done
uw=.deps/uwebsockets
us=.deps/usockets
sqlite=.deps/sqlite-amalgamation-3530400
[[ -f $sqlite/sqlite3.c ]] || unzip -q .deps/sqlite.zip -d .deps
# uWebSockets exposes no HTTP keep-alive setting; extend both pinned defaults.
for pair in 'HttpResponse.h HTTP_TIMEOUT_S' 'HttpContext.h HTTP_IDLE_TIMEOUT_S'; do
  read -r file constant <<< "$pair"
  awk -v key="$constant" '
    $0 ~ "static const int " key " = (10|75);" { sub(/= (10|75);/, "= 75;"); count++ }
    { print }
    END { if (count != 1) exit 1 }
  ' "$uw/src/$file" > "$uw/src/$file.tmp"
  mv "$uw/src/$file.tmp" "$uw/src/$file"
done
flags=(-O3 -DNDEBUG -std=gnu11 -pthread)
if [[ ! -f .deps/sqlite3.o || build.sh -nt .deps/sqlite3.o || $sqlite/sqlite3.c -nt .deps/sqlite3.o ]]; then
  "${CC:-cc}" "${flags[@]}" -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
    -c "$sqlite/sqlite3.c" -o .deps/sqlite3.o
fi
if [[ ! -f $us/uSockets.a || build.sh -nt $us/uSockets.a ]]; then
  (cd "$us"
    "${CC:-cc}" "${flags[@]}" -DLIBUS_NO_SSL -Isrc -c src/*.c src/eventing/*.c src/crypto/*.c src/io_uring/*.c
    "${AR:-ar}" rcs uSockets.a ./*.o)
fi
# fio-stl uses C99 initializer extensions supported by Clang's C++ frontend.
"${CXX:-clang++}" -O3 -DNDEBUG -std=gnu++20 -pthread -Wno-c99-designator \
  -DLIBUS_NO_SSL -DUWS_NO_ZLIB -DUWS_HTTPRESPONSE_NO_WRITEMARK \
  -I.deps -I"$sqlite" -I"$uw/src" -I"$us/src" \
  server.cpp .deps/sqlite3.o "$us/uSockets.a" -lm -o bin/server
