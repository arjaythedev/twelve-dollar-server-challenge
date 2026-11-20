#!/usr/bin/env bash
# vm_build.sh VARIANT... — build the 1-vCPU guest used by vm153.py (benchmark host, root; run
# under /root/twelve/bench.lock). Nothing is installed on the host itself:
#   - image l153-vmhost:1 — ubuntu:24.04 + qemu-system-x86, iproute2, python3 (runs qemu, the
#     tap device and the load generator in its own network namespace);
#   - vm/rootfs.img — ubuntu:24.04 + iproute2/procps, vm_init.sh as /init.sh, the seed database at
#     /work/feed.db (UID 65534) and artifacts/VARIANT/server for each VARIANT;
#   - vm/vmlinuz-6.8.0-{100,149}-generic — the host's Ubuntu 24.04 kernels (virtio, ext4 built in).
set -euo pipefail
R=/root/twelve/lean-153-improve; C=/root/twelve/challenge; V=$R/vm
mkdir -p $V; cd $V
img() { docker image inspect "$1" > /dev/null 2>&1 || printf '%s\n' "FROM ubuntu:24.04" \
  "RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $2 && rm -rf /var/lib/apt/lists/*" \
  | docker build -q -t "$1" -; }
img l153-vmhost:1 "qemu-system-x86 iproute2 python3 procps util-linux"
img l153-vmguest:2 "iproute2 procps linux-tools-6.8.0-100-generic linux-tools-6.8.0-149-generic"   # :2 adds perf (guest profiling)
rm -rf root; mkdir root
cid=$(docker create l153-vmguest:2); docker export "$cid" | tar -x -C root; docker rm "$cid" > /dev/null
install -m 755 $R/harness/vm_init.sh root/init.sh
mkdir -p root/work root/variants
cp $C/seed/feed.db root/work/feed.db; chown -R 65534:65534 root/work
for v in "$@"; do install -D -m 755 $R/artifacts/$v/server root/variants/$v/server; done
rm -f rootfs.img.new; truncate -s 6G rootfs.img.new; mkfs.ext4 -q -F -d root rootfs.img.new; mv rootfs.img.new rootfs.img
rm -rf root
for k in 6.8.0-100-generic 6.8.0-149-generic; do [ -f vmlinuz-$k ] || cp /boot/vmlinuz-$k vmlinuz-$k; done
ls -la $V; echo "variants: $*"
