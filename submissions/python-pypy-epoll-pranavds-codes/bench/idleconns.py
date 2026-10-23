# Open N keep-alive connections, send one request on each, read the reply, and hold them open.
import selectors, socket, sys, time
port, n, hold = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
socks = []
for i in range(n):
    s = socket.socket(); s.connect(("127.0.0.1", port)); s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n"); socks.append(s)
    if i % 2000 == 1999:
        for t in socks[-2000:]: t.recv(65536)
for t in socks[-(n % 2000 or 2000):]:
    pass
print("opened", n, flush=True)
time.sleep(hold)
