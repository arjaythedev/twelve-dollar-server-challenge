#!/usr/bin/env bash
# Builds as a normal user: the Kotlin server (build/app jars) and libsqlite3 from the SQLite 3.54.0 amalgamation,
# which the build downloads and checks against the SHA3-256 sqlite.org publishes for it. TrikeShed comes from the git
# subrepo trikeshed/TrikeShed: a checkout without submodules leaves it empty, and then it is fetched here at the
# commit git records for it (TRIKESHED_URL overrides the repository).
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -d trikeshed/TrikeShed/src ]; then
  commit=$(git ls-files --stage -- trikeshed/TrikeShed 2> /dev/null | awk '$1 == "160000" { print $2 }') || true
  [ -n "$commit" ] || { echo "git records no commit for the subrepo trikeshed/TrikeShed" >&2; exit 1; }
  mkdir -p trikeshed/TrikeShed
  git -C trikeshed/TrikeShed init -q
  git -C trikeshed/TrikeShed fetch -q --depth 1 "${TRIKESHED_URL:-https://github.com/jnorthrup/TrikeShed.git}" "$commit"
  git -C trikeshed/TrikeShed checkout -q --detach FETCH_HEAD
fi
if [ -x /opt/graalvm/bin/java ]; then export JAVA_HOME=/opt/graalvm; fi
GRADLE=${GRADLE:-/opt/gradle/bin/gradle}; [ -x "$GRADLE" ] || GRADLE=gradle
"$GRADLE" --no-daemon --quiet --console=plain :server:app :server:sqliteLib
