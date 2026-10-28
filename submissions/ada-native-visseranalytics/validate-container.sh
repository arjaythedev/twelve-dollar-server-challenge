#!/usr/bin/env bash
# Local correctness checks; all compilation and DB writes stay inside the container.
set -euo pipefail
cd "$(dirname "$0")"
if [[ -d /Applications/Docker.app/Contents/Resources/bin ]]; then
  export PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi
SUBMISSION=$(pwd)
CHALLENGE=$(cd ../.. && pwd)
test -s "$CHALLENGE/seed/feed.db" || { echo 'Missing seed/feed.db' >&2; exit 1; }
docker info >/dev/null
IMAGE=${ADA_VALIDATION_IMAGE:-"ada-validation:$(printf '%s' "$SUBMISSION" | shasum -a 256 | cut -c1-12)"}
mkdir -p validation-artifacts
LOG="$SUBMISSION/validation-artifacts/container-$(date -u +%Y%m%dT%H%M%SZ)-$$.log"
printf 'Validation log: %s\n' "$LOG"
{
  # Reuse a previously built pinned toolchain when an Ubuntu mirror no longer
  # carries an exact package revision. Application sources are ALWAYS rebuilt
  # from the read-only worktree copy below.
  if [[ ${ADA_VALIDATION_REUSE_IMAGE:-0} == 1 ]]; then
    docker image inspect "$IMAGE" --format 'Reusing validation toolchain {{.Id}}'
  else
    docker build --platform linux/amd64 --target validation --tag "$IMAGE" .
  fi
  docker run --rm --platform "${ADA_VALIDATION_PLATFORM:-linux/amd64}" --cpus=1 --memory=2g --memory-swap=2g \
    --ulimit nofile=65535:65535 --entrypoint bash \
    -e "ADA_VALIDATE_DIRECT_ONLY=${ADA_VALIDATE_DIRECT_ONLY:-0}" \
    --mount "type=bind,source=$CHALLENGE,target=/input,readonly" \
    --workdir /work "$IMAGE" -euc '
      mkdir -p seed bench submissions/ada-native-visseranalytics
      cp /input/seed/feed.db seed/
      cp /input/bench/nginx.conf bench/
      cp -R /input/test .
      cp /input/schema.sql .
      cp -R /input/submissions/ada-native-visseranalytics/src /input/submissions/ada-native-visseranalytics/tests submissions/ada-native-visseranalytics/
      cp /input/submissions/ada-native-visseranalytics/build.sh /input/submissions/ada-native-visseranalytics/start.sh /input/submissions/ada-native-visseranalytics/versions.env submissions/ada-native-visseranalytics/
      chown -R ada:ada /work
      chmod -R u+rwX /work
      chmod u+x submissions/ada-native-visseranalytics/start.sh
      runuser -u ada -- python3 submissions/ada-native-visseranalytics/tests/json_strings.py
      runuser -u ada -- env PORT=3001 bash test/run.sh submissions/ada-native-visseranalytics
      runuser -u ada -- env PORT=3001 python3 submissions/ada-native-visseranalytics/tests/focused.py
      if [[ $ADA_VALIDATE_DIRECT_ONLY != 1 ]]; then
        runuser -u ada -- python3 submissions/ada-native-visseranalytics/tests/nginx_check.py
      fi
      runuser -u ada -- python3 submissions/ada-native-visseranalytics/tests/lifecycle.py
      runuser -u ada -- python3 submissions/ada-native-visseranalytics/tests/feed_ranges.py
      dpkg-query -W gnat-13 gcc-13 libgnat-13 libssl3t64
      uname -a
    '
} 2>&1 | tee "$LOG"
