#!/usr/bin/env bash
# Inside a fresh ubuntu:24.04 container; mounts are read-only and all work is disposable.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq sqlite3 jq curl openssl ca-certificates >/dev/null
mkdir -p /work/seed
cp -a /input /work/submission
rm -rf /work/submission/spec/.lake /work/submission/build /work/submission/bin /work/submission/vendor
cp -a /tests /work/test
cp /seed/feed.db /seed/tokens.json /work/seed/
useradd --uid 10001 --create-home bench
chown -R 10001:10001 /work
bash /work/submission/install.sh
runuser -u bench -- bash -c 'cd /work && id && bash test/run.sh submission'
