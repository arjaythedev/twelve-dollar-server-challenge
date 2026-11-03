#!/usr/bin/env bash
# Builds as a normal user: the Kotlin/Native server (server/build/bin/linuxX64/feedReleaseExecutable/feed.kexe, the
# runtime start.sh serves by default) with SQLite 3.54.0 linked in from its amalgamation, which the build downloads and
# checks against the SHA3-256 sqlite.org publishes for it. The server imports TrikeShed as a library,
# borg.trikeshed:TrikeShed:0.1.0-20280e867, from the Maven repository settings.gradle.kts names (branch trikeshed-m2 of
# this fork, every target, built by TrikeShed's own build), so this build does not compile TrikeShed. The git checkout
# trikeshed/TrikeShed (fetched here at the pinned commit when empty; TRIKESHED_URL overrides the repository) supplies
# liburing's sources and the load generator's module.
# The other runtimes (graal-static, graal-jit, node, wasi) build from the same tree with bench/all1000.sh.
#
# Run the SQLite downloader, native compile and release link in separate Gradle JVMs so clang has room.
set -euo pipefail
# glibc keeps one malloc arena per thread; two bound the Gradle JVMs' and the Kotlin/Native compiler's native memory.
export MALLOC_ARENA_MAX=2
cd "$(dirname "$0")"
TRIKESHED_COMMIT=20280e86761b3df64cedd4579e311c04411d22b4
if [ ! -d trikeshed/TrikeShed/src ]; then
  commit=$TRIKESHED_COMMIT
  mkdir -p trikeshed/TrikeShed
  git -C trikeshed/TrikeShed init -q
  git -C trikeshed/TrikeShed fetch -q --depth 1 "${TRIKESHED_URL:-https://github.com/jnorthrup/TrikeShed.git}" "$commit"
  git -C trikeshed/TrikeShed checkout -q --detach FETCH_HEAD
fi
if [ -x /opt/graalvm/bin/java ]; then export JAVA_HOME=/opt/graalvm; fi
GRADLE=${GRADLE:-/opt/gradle/bin/gradle}; [ -x "$GRADLE" ] || GRADLE=gradle
gradle=("$GRADLE" --no-daemon --console=plain --configure-on-demand)
"${gradle[@]}" :server:sqliteSource
"${gradle[@]}" -Ptrike.targets=linuxX64 :server:compileKotlinLinuxX64
exec "${gradle[@]}" -Ptrike.targets=linuxX64 :server:linkFeedReleaseExecutableLinuxX64
