#!/usr/bin/env bash
# Builds as a normal user: the Kotlin server (build/app jars) and libsqlite3 from the SQLite 3.54.0 amalgamation,
# which the build downloads and checks against the SHA3-256 sqlite.org publishes for it. TrikeShed comes from the git
# checkout trikeshed/TrikeShed: when it is empty, it is fetched here at the pinned commit below
# (TRIKESHED_URL overrides the repository).
#
# Resource limits (rule 8): the challenge box is a DigitalOcean Basic droplet, Ubuntu 24.04 x86_64, 1 vCPU, 2 GB RAM,
# no swap. This build and the server share that CPU and RAM with the OS. Gradle runs with a 1 GB heap and one worker
# (gradle.properties); start.sh runs the server with a 384 MB heap.
# The same limits were applied for the clean-room test run, with cgroup v2 in an ubuntu:24.04 container on
# a WSL2 kernel, not the droplet's kernel:
#   docker run --platform linux/amd64 --cpus=1 --memory=2g --memory-swap=2g ubuntu:24.04
#   (cpu.max=100000 100000, memory.max=2147483648, memory.swap.max=0)
# The CPU limit is a quota, not a pinned core (rule 11). Timings from that run are not the droplet's timings.
set -euo pipefail
cd "$(dirname "$0")"
TRIKESHED_COMMIT=17c7180dd2303280e5889abb2a3f690064a55bf8
if [ ! -d trikeshed/TrikeShed/src ]; then
  commit=$TRIKESHED_COMMIT
  mkdir -p trikeshed/TrikeShed
  git -C trikeshed/TrikeShed init -q
  git -C trikeshed/TrikeShed fetch -q --depth 1 "${TRIKESHED_URL:-https://github.com/jnorthrup/TrikeShed.git}" "$commit"
  git -C trikeshed/TrikeShed checkout -q --detach FETCH_HEAD
fi
if [ -x /opt/graalvm/bin/java ]; then export JAVA_HOME=/opt/graalvm; fi
GRADLE=${GRADLE:-/opt/gradle/bin/gradle}; [ -x "$GRADLE" ] || GRADLE=gradle
"$GRADLE" --no-daemon --quiet --console=plain :server:app :server:sqliteLib
