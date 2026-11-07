#!/usr/bin/env bash
# install.sh — runs once as root on a clean Ubuntu 24.04, before build.sh.
#
# READ THE README FIRST. This entry cannot run under the stock runtime contract: the server runs as an unprivileged
# user with only CAP_NET_BIND_SERVICE, and Ubuntu 24.04's stock kernel.unprivileged_bpf_disabled=2 means it cannot
# load a single BPF program. Loading sk_skb/sockmap/sockops needs CAP_BPF + CAP_NET_ADMIN — a privilege escalation
# beyond the contract. This install step therefore installs only the build toolchain and does NOT grant any
# capability to the shim. That is deliberate: the honest state of this entry is "does not load on the box as run."
# See the README's "Privileges" section. This is a novelty / rules-probe entry; it needs an organizer ruling.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential clang llvm libbpf-dev libelf-dev zlib1g-dev curl unzip ca-certificates libcap2-bin

echo "install.sh: toolchain installed."
echo "NOTE: granting CAP_BPF+CAP_NET_ADMIN to the shim is OUT OF CONTRACT and is not done here (see README)."
