#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Temurin JDK 27 (the newest OpenJDK release) and sbt 2.0.11, both from
# their official release tarballs, checksum-verified. SQLite comes with the sqlite-jdbc driver (its native library is
# bundled in the jar).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl

case "$(uname -m)" in
  x86_64)  JDK_ARCH=x64;     JDK_SHA=1cf69a4848ffb728b3b260dfd45206a51566ab571a02a30092271d4c580bccbc ;;
  aarch64) JDK_ARCH=aarch64; JDK_SHA=e4ec5c7276290c7bde0baecba9a29be4962f5388684296406a9799d465936087 ;;
  *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/jdk.tgz" "https://github.com/adoptium/temurin27-binaries/releases/download/jdk-27%2B35/OpenJDK27U-jdk_${JDK_ARCH}_linux_hotspot_27_35.tar.gz"
echo "$JDK_SHA  $tmp/jdk.tgz" | sha256sum -c -
rm -rf /opt/jdk-27 && mkdir -p /opt/jdk-27 && tar -xzf "$tmp/jdk.tgz" -C /opt/jdk-27 --strip-components=1

curl -fsSL -o "$tmp/sbt.tgz" https://github.com/sbt/sbt/releases/download/v2.0.11/sbt-2.0.11.tgz
echo "d8409a244e97c3839f27c4f435f49421196c126a450d30f4cf53522d39e3c379  $tmp/sbt.tgz" | sha256sum -c -
rm -rf /opt/sbt && mkdir -p /opt/sbt && tar -xzf "$tmp/sbt.tgz" -C /opt/sbt --strip-components=1

/opt/jdk-27/bin/java -version
