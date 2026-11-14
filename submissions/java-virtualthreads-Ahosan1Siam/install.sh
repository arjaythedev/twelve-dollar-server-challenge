#!/usr/bin/env bash
set -euo pipefail

# Ensure curl and JDK are available
if ! command -v curl >/dev/null 2>&1; then
  apt-get update -y && apt-get install -y curl
fi

if ! command -v java >/dev/null 2>&1 || ! command -v javac >/dev/null 2>&1; then
  apt-get update -y && apt-get install -y openjdk-21-jdk-headless
fi

# Download sqlite-jdbc if not already present
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ ! -f "$DIR/sqlite-jdbc.jar" ]; then
  echo "Downloading sqlite-jdbc..."
  curl -L -o "$DIR/sqlite-jdbc.jar" "https://repo1.maven.org/maven2/org/xerial/sqlite-jdbc/3.49.1.0/sqlite-jdbc-3.49.1.0.jar"
fi

