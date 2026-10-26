#!/usr/bin/env bash
# build.sh: Builds the app as a normal user.
set -euo pipefail

cd "$(dirname "$0")"

JDK21_HOME=""
for jdir in \
  /usr/lib/jvm/java-21-openjdk-amd64 \
  /usr/lib/jvm/java-21-openjdk-arm64 \
  "${JAVA_HOME_21_X64:-}" \
  "${JAVA_HOME_21_ARM64:-}" \
  /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
  /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home
do
  if [ -n "$jdir" ] && [ -x "$jdir/bin/javac" ]; then
    JDK21_HOME="$jdir"
    break
  fi
done

if [ -z "$JDK21_HOME" ] && command -v /usr/libexec/java_home >/dev/null 2>&1; then
  JH="$(/usr/libexec/java_home -v 21 2>/dev/null || /usr/libexec/java_home 2>/dev/null || true)"
  if [ -n "$JH" ] && [ -x "$JH/bin/javac" ]; then
    JDK21_HOME="$JH"
  fi
fi

if [ -n "$JDK21_HOME" ]; then
  export JAVA_HOME="$JDK21_HOME"
  export PATH="$JAVA_HOME/bin:$PATH"
fi

mvn clean package -DskipTests --batch-mode --no-transfer-progress
