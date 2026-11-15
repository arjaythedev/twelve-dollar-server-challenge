#!/usr/bin/env bash
# ci153.sh SRC_DIR LABEL — the upstream CI flow (install.sh as root, then test/run.sh as a normal
# user) in a fresh ubuntu:24.04 container: one CPU, 2 GiB, LimitNOFILE 65535, the challenge's
# test/ and seed mounted read-only. io_uring needs a seccomp exception (no privileged mode).
set -euo pipefail
src=$(cd "${1:?source dir}" && pwd); label=${2:?label}
R=${STUDY_ROOT:-/root/twelve/lean-153-improve}
out=$R/results/ci-$label-$(date +%s); mkdir -p "$out"
cat > "$out/inner.sh" <<'IN'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq sqlite3 jq curl openssl ca-certificates >/dev/null
mkdir -p /work/seed /work/submissions
cp -a /input /work/submissions/sub
rm -rf /work/submissions/sub/spec/.lake /work/submissions/sub/build /work/submissions/sub/bin /work/submissions/sub/vendor
cp -a /tests /work/test
cp /seed/feed.db /seed/tokens.json /work/seed/
useradd --uid 10001 --create-home bench
chown -R 10001:10001 /work
bash /work/submissions/sub/install.sh
runuser -u bench -- bash -c 'cd /work && bash test/run.sh submissions/sub'
IN
rc=0
docker run --rm --name "l153-ci-$label-$$" --security-opt seccomp=unconfined --cpuset-cpus=0 \
  --memory=2g --memory-swap=2g --ulimit nofile=65535:65535 \
  --mount "type=bind,source=$src,target=/input,readonly" \
  --mount type=bind,source=/root/twelve/challenge/test,target=/tests,readonly \
  --mount type=bind,source=/root/twelve/challenge/seed,target=/seed,readonly \
  --mount "type=bind,source=$out/inner.sh,target=/inner.sh,readonly" \
  ubuntu:24.04 bash /inner.sh > "$out/ci.log" 2>&1 || rc=$?
echo "$rc" > "$out/exit-code.txt"
tail -6 "$out/ci.log"
echo "CI rc=$rc logs=$out"
exit $rc
