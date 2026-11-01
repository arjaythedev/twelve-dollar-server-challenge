#!/usr/bin/env bash
set -euxo pipefail
cd "$(dirname "$0")"

rm -rf build
rm -f server.c bin/server

mkdir -p build bin

javac -d build Compiler/CompilerSparseTape.java
java -cp build CompilerSparseTape

gcc -O3 -march=native -flto -pipe -o bin/server "handler/handler_passthrough_edgewake.c" server.c -lsqlite3 -lssl -lcrypto
