#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
source ./java-env.sh
MAVEN=.tools/apache-maven-3.9.11
if [[ ! -x "$MAVEN/bin/mvn" ]]; then
  mkdir -p .tools
  archive=$(mktemp)
  trap 'rm -f "$archive"' EXIT
  curl -fL --retry 3 'https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.11/apache-maven-3.9.11-bin.tar.gz' -o "$archive"
  echo "bcfe4fe305c962ace56ac7b5fc7a08b87d5abd8b7e89027ab251069faebee516b0ded8961445d6d91ec1985dfe30f8153268843c89aa392733d1a3ec956c9978  $archive" | sha512sum -c -
  tar -xzf "$archive" -C .tools
  rm -f "$archive"
  trap - EXIT
fi
# Keep the Maven repository inside the ignored tools directory for normal-user builds.
"$MAVEN/bin/mvn" -B -ntp -Dmaven.repo.local="$PWD/.tools/repository" clean package

python3 build-native.py
