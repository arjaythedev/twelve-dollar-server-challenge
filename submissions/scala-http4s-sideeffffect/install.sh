#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: Temurin JDK 27 (the newest OpenJDK release) and Scala CLI 1.18.0, both from
# their official releases, checksum-verified. SQLite comes with the sqlite-jdbc driver (its native library is
# bundled in the jar).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl

case "$(uname -m)" in
  x86_64)  JDK_ARCH=x64;     SCLI_SHA=eb41442f870bcae75aa5d6622ac96a173dae14fed59f71c97125eb21536bb060
           JDK_SHA=1cf69a4848ffb728b3b260dfd45206a51566ab571a02a30092271d4c580bccbc ;;
  aarch64) JDK_ARCH=aarch64; SCLI_SHA=a9e5d10b4bb623839d1697518102d5b6a81d8f9ed8031b10b97f87f35a63ae68
           JDK_SHA=e4ec5c7276290c7bde0baecba9a29be4962f5388684296406a9799d465936087 ;;
  *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/jdk.tgz" "https://github.com/adoptium/temurin27-binaries/releases/download/jdk-27%2B35/OpenJDK27U-jdk_${JDK_ARCH}_linux_hotspot_27_35.tar.gz"
echo "$JDK_SHA  $tmp/jdk.tgz" | sha256sum -c -
rm -rf /opt/jdk-27 && mkdir -p /opt/jdk-27 && tar -xzf "$tmp/jdk.tgz" -C /opt/jdk-27 --strip-components=1

# Scala CLI: one native binary that compiles the app (build.sh); it fetches Scala and the dependencies itself.
curl -fsSL -o "$tmp/scala-cli.gz" "https://github.com/VirtusLab/scala-cli/releases/download/v1.18.0/scala-cli-$(uname -m)-pc-linux.gz"
echo "$SCLI_SHA  $tmp/scala-cli.gz" | sha256sum -c -
rm -rf /opt/scala-cli && mkdir -p /opt/scala-cli && gunzip -c "$tmp/scala-cli.gz" > /opt/scala-cli/scala-cli
chmod 755 /opt/scala-cli/scala-cli

/opt/jdk-27/bin/java -version
