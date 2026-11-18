#!/usr/bin/env bash
# runvm153.sh VARIANT [vm153.py options] — wait for a quiet host, hold the shared benchmark lock and
# run one 1-vCPU guest measurement (vm153.py) in the l153-vmhost container: privileged for KVM, tap
# and vhost-net, with its own network namespace (--network none), so the host's network and
# settings are untouched. The guest disk overlay lives on a tmpfs inside the container.
set -euo pipefail
R=${STUDY_ROOT:-/root/twelve/lean-153-improve}
exec flock -w 7200 /root/twelve/bench.lock bash -c '
  python3 "'"$R"'/harness/wait_quiet.py" 10
  exec docker run --rm --init --privileged --network none --ulimit nofile=1048576:1048576 \
    --tmpfs /vmtmp:size=6g -e TMPDIR=/vmtmp -v /root/twelve:/root/twelve \
    l153-vmhost:1 python3 "'"$R"'/harness/vm153.py" "$@"' bash "$@"
