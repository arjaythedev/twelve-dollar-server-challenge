"""Hold a bounded set of ordinary handshakes to the disposable private pressure-test listener."""

import os
import resource
import socket
import sys

soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (max(4096, soft), hard))
peers = []
for _ in range(2304):
    sock = socket.create_connection(("198.18.203.1", 19494), timeout=4)
    peers.append(sock)
print("READY 2304", flush=True)
sys.stdin.readline()
for s in peers:
    s.close()
