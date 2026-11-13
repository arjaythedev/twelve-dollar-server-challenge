#!/usr/bin/env bash
# start.sh — runs the shim in the foreground. The shim loads bin/http.bpf.o (relative to here) and reads its config
# from SQLITE_PATH, JWT_SECRET, HOST, PORT. Needs CAP_BPF + CAP_NET_ADMIN on the binary (see README / install.sh):
# loading sk_skb/sockmap/sockops programs is impossible under the plain CAP_NET_BIND_SERVICE the contract grants.
set -euo pipefail
cd "$(dirname "$0")/bin"
exec ./shim
