#!/usr/bin/env bash
# Builds the app as a normal user. Everything native is compiled from pinned source on the box:
#   - uWebSockets.js (+ uWebSockets, uSockets) fetched by exact git commit, built without TLS/QUIC
#     and with the HTTP idle timeout raised from a hard-coded 10 s to 75 s (see README.md)
#   - better-sqlite3 from npm (package-lock.json), compiled with its bundled SQLite amalgamation
set -euo pipefail
cd "$(dirname "$0")"
HERE="$PWD"
NODE_PREFIX=/opt/node-v24.21.0 # from install.sh
export PATH="$NODE_PREFIX/bin:$PATH"

UWS_JS=127ac8029b04e8272da068055bc04c1cd0d7bd38   # uWebSockets.js v20.71.0 (source_commit of the release)
UWS=1d80f7a53816ef2433e89793b285b827f3efb2b2      # its uWebSockets submodule
USOCKETS=86097c490263ab662d62e8e7b541390bdec7d149 # its uSockets submodule
NODE_VERSION=v24.21.0
FAST_API_SHA=c6c8b22ebf8014ef5b1cb04bec2af7549754cede52b745474aae1762e060b842 # v8-fast-api-calls.h of that release
NODE_INC="$NODE_PREFIX/include/node"

fetch() { # fetch <dir> <github repo> <commit>: a shallow checkout of exactly that commit
  rm -rf "$1"; git init -q "$1"
  git -C "$1" fetch -q --depth 1 "https://github.com/$2.git" "$3"
  git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
  [ "$(git -C "$1" rev-parse HEAD)" = "$3" ]
}

mkdir -p build bin
cd build
if [ ! -f "uws.$UWS_JS.ok" ]; then
  fetch uwsjs uNetworking/uWebSockets.js "$UWS_JS"
  fetch uwsjs/uWebSockets uNetworking/uWebSockets "$UWS"
  fetch uwsjs/uWebSockets/uSockets uNetworking/uSockets "$USOCKETS"
  touch "uws.$UWS_JS.ok"
fi

# v8-fast-api-calls.h is not in Node's header tarball; take it from the same Node release's source.
mkdir -p include
curl -fsSL "https://raw.githubusercontent.com/nodejs/node/$NODE_VERSION/deps/v8/include/v8-fast-api-calls.h" -o include/v8-fast-api-calls.h
echo "$FAST_API_SHA  include/v8-fast-api-calls.h" | sha256sum -c - >/dev/null

# Patched copies of the sources, so a rebuild always starts from the pinned files.
rm -rf src && cp -r uwsjs/src src && cp -r uwsjs/uWebSockets/src src/uws && cp -r uwsjs/uWebSockets/uSockets/src src/us
# 1. Keep idle keep-alive connections for 75 s instead of 10 s (k6 users think for up to 23 s between
#    requests, and SPEC.md asks for at least 65 s). Both constants matter: HTTP_IDLE_TIMEOUT_S covers
#    a connection waiting for its next request, and HTTP_TIMEOUT_S is what a socket is left with after
#    each response is written.
sed -i 's/static const int HTTP_IDLE_TIMEOUT_S = 10;/static const int HTTP_IDLE_TIMEOUT_S = 75;/' src/uws/HttpContext.h
sed -i 's/static const int HTTP_TIMEOUT_S = 10;/static const int HTTP_TIMEOUT_S = 75;/' src/uws/HttpResponse.h
grep -q 'HTTP_IDLE_TIMEOUT_S = 75;' src/uws/HttpContext.h && grep -q 'HTTP_TIMEOUT_S = 75;' src/uws/HttpResponse.h
# 2. No TLS and no HTTP/3, so no BoringSSL + lsquic to build: drop the H3App export and the HTTP/3
#    request/response templates. What's left only names the H3 types in `std::is_same` checks and in
#    `if constexpr` branches that are never instantiated, so forward declarations are enough.
sed -i '/#include "Http3App.h"/d; /"H3App"/d; /reqTemplate\[1\]\.Reset/d; /resTemplate\[2\]\.Reset/d' src/addon.cpp
! grep -q -e 'H3App", NewStringType' -e 'init<2>' src/addon.cpp
cat > include/h3stub.h <<'EOF'
namespace uWS { struct H3App; struct Http3Request; struct Http3Response; }
EOF

DEFS="-DLIBUS_USE_LIBUV -DLIBUS_NO_SSL -DUWS_WITH_PROXY -DUWS_HTTPRESPONSE_NO_WRITEMARK"
OPT="-O3 -flto=auto -fPIC -fno-plt -DNDEBUG"
rm -f ./*.o
cc  $DEFS $OPT -I src/us -I "$NODE_INC" -c src/us/*.c src/us/eventing/*.c
c++ $DEFS $OPT -std=c++20 -include include/h3stub.h -I include -I src/us -I src/uws -I "$NODE_INC" -c src/addon.cpp
c++ $OPT -shared -o "$HERE/bin/uws.node" ./*.o -static-libstdc++ -static-libgcc -s
cd "$HERE"

# better-sqlite3 (pinned in package-lock.json). Its npm package ships prebuilt addons: delete them and
# compile the addon and its bundled SQLite amalgamation from source with node-gyp (bundled with npm).
npm ci --no-audit --no-fund --ignore-scripts --loglevel=error
rm -rf node_modules/better-sqlite3/prebuilds
(cd node_modules/better-sqlite3 &&
  node "$NODE_PREFIX/lib/node_modules/npm/node_modules/node-gyp/bin/node-gyp.js" rebuild --release \
    --force_build=1 --nodedir="$NODE_PREFIX" --jobs=max --loglevel=warn)
cp node_modules/better-sqlite3/build/Release/better_sqlite3.node bin/
node -e "require('./bin/uws.node'); new (require('better-sqlite3'))(':memory:', { nativeBinding: './bin/better_sqlite3.node' })"
echo "build ok"
