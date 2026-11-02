#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# RUNTIME picks the build (build.sh makes all five): kn (default, Kotlin/Native over io_uring direct descriptors),
# graal-static (GraalVM native image), graal-jit (GraalVM JDK), node (Kotlin/JS on Node.js), wasi (Kotlin/Wasm on
# wasmtime). A runtime whose build is missing falls back along kn, graal-static, graal-jit. The GraalVM builds serve
# through the io_uring ring (TRANSPORT=ring) unless TRANSPORT says otherwise; Node and wasmtime poll their sockets.
set -euo pipefail
cd "$(dirname "$0")"
KN=server/build/bin/linuxX64/feedReleaseExecutable/feed.kexe
STATIC=server/build/native/feed-static
NODE_MAIN=server/build/dist/node/server.js
WASI_MAIN=server/build/dist/wasi/server.wasm
LIB="$PWD/server/build/sqlite/libsqlite3.so"; [ -f "$LIB" ] || LIB="$PWD/server/build/sqlite/libsqlite3.dylib"
JAVA=/opt/graalvm/bin/java; [ -x "$JAVA" ] || JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
runtime=${RUNTIME:-kn}
[ "$runtime" = kn ] && [ ! -x "$KN" ] && runtime=graal-static
[ "$runtime" = graal-static ] && [ ! -x "$STATIC" ] && runtime=graal-jit
echo "start.sh: RUNTIME=$runtime" >&2
case "$runtime" in
  kn)
    exec "$KN" ;;
  graal-static)
    export TRANSPORT=${TRANSPORT:-ring}
    exec "$STATIC" -Xms384m -Xmx384m -Dsqlite.lib="$LIB" ;;
  graal-jit)
    export TRANSPORT=${TRANSPORT:-ring}
    exec "$JAVA" -XX:+UseSerialGC -Xms384m -Xmx384m -XX:-UsePerfData \
      --enable-native-access=ALL-UNNAMED -Dsqlite.lib="$LIB" \
      -cp "server/build/app/*" borg.trikeshed.feed.MainKt ;;
  node)
    NODE=/opt/node/bin/node; [ -x "$NODE" ] || NODE=node
    exec "$NODE" "$NODE_MAIN" ;;
  wasi)
    WASMTIME=/opt/wasmtime/wasmtime; [ -x "$WASMTIME" ] || WASMTIME=wasmtime
    exec "$WASMTIME" run -W gc=y,function-references=y,exceptions=y -S tcplisten="${HOST:-0.0.0.0}:${PORT:-80}" \
      --dir "$(dirname "$SQLITE_PATH")" --env SQLITE_PATH --env JWT_SECRET "$WASI_MAIN" ;;
  *)
    echo "start.sh: RUNTIME must be kn, graal-static, graal-jit, node or wasi (got $runtime)" >&2; exit 2 ;;
esac
