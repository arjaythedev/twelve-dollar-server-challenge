#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/build/classes" "$DIR/build/fatjar"

# Download sqlite-jdbc if needed
if [ ! -f "$DIR/sqlite-jdbc.jar" ]; then
  curl -L -o "$DIR/sqlite-jdbc.jar" "https://repo1.maven.org/maven2/org/xerial/sqlite-jdbc/3.49.1.0/sqlite-jdbc-3.49.1.0.jar"
fi

# Compile all sources with release 21
javac -cp "$DIR/sqlite-jdbc.jar" -d "$DIR/build/classes" "$DIR"/src/server/*.java

# Build single self-contained executable fat JAR
(
  cd "$DIR/build/fatjar"
  jar -xf "$DIR/sqlite-jdbc.jar"
  cp -r "$DIR/build/classes/"* .
  jar -cfe "$DIR/build/server.jar" server.Server *
)

echo "Build complete: $DIR/build/server.jar"

