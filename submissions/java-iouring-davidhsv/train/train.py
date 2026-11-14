#!/usr/bin/env python3
"""Profile training for build.sh (build time only; nothing here is in bin/server).

Starts the server with the instrumented libfeed.so on a synthetic database made here (never the
challenge seed), with tokens signed here, and drives it the way bench/load.js's users do: read
the feed, read one of its posts, sometimes like it, rarely write a post. A few malformed and
unauthorized requests cover the error paths. Then SIGTERM: the JVM exits through exit(), and the
library writes its profile.

    python3 train/train.py ./start.sh
"""
import base64, hashlib, hmac, json, os, random, signal, socket, sqlite3, subprocess, sys, tempfile, time

SCHEMA = """
CREATE TABLE users (
  id          INTEGER PRIMARY KEY,
  username    TEXT NOT NULL UNIQUE,
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE posts (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE likes (
  user_id     INTEGER NOT NULL REFERENCES users(id),
  post_id     INTEGER NOT NULL REFERENCES posts(id),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  PRIMARY KEY (user_id, post_id)
);
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx       ON posts (user_id);
CREATE INDEX likes_post_id_idx       ON likes (post_id);
"""

SECRET = b"a-secret-for-training-only"
USERS, POSTS, LIKES = 30000, 150000, 600000
CONNECTIONS, ROUNDS = 128, 500
NOUNS = "heron marsh ember quartz lantern tide orchard compass willow anvil comet harbor".split()


def make_database(path):
    rnd = random.Random(12)
    db = sqlite3.connect(path)
    db.executescript("PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;" + SCHEMA)
    stamp = lambda t: time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % (int(t * 1000) % 1000)
    db.executemany("INSERT INTO users VALUES (?, ?, ?)",
                   ((i, "%s_%s_%d" % (rnd.choice(NOUNS), rnd.choice(NOUNS), i), stamp(1.7e9 + i)) for i in range(1, USERS + 1)))
    def posts():
        t = 1.71e9
        for i in range(1, POSTS + 1):
            t += rnd.random() * 90
            body = " ".join(rnd.choice(NOUNS) for _ in range(rnd.randint(4, 30)))
            if i % 499 == 0:
                body += ' "said" the\ttraveller \\ café ✓'
            yield i, rnd.randint(1, USERS), body, stamp(t)
    db.executemany("INSERT INTO posts VALUES (?, ?, ?, ?)", posts())
    pairs = set()
    while len(pairs) < LIKES:
        pairs.add((rnd.randint(1, USERS), rnd.randint(1, POSTS)))
    db.executemany("INSERT INTO likes (user_id, post_id) VALUES (?, ?)", sorted(pairs))
    db.commit()
    db.execute("PRAGMA journal_mode = WAL")
    db.close()


def b64(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=")


def sign(claims, alg="HS256"):
    signed = b64(json.dumps({"alg": alg, "typ": "JWT"}, separators=(",", ":")).encode()) + b"." + \
        b64(json.dumps(claims, separators=(",", ":")).encode())
    return (signed + b"." + b64(hmac.new(SECRET, signed, hashlib.sha256).digest())).decode()


def token(uid, **extra):
    now = int(time.time())
    return sign({"sub": str(uid), "username": "user_%d" % uid, "iat": now, "exp": now + 3600, **extra})


def request(method, path, auth=None, body=None):
    head = "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: Grafana k6/1.3.0\r\n" % (method, path)
    if method == "POST":
        head += "Content-Length: %d\r\n" % len(body or b"")
    if auth:
        head += "Authorization: %s\r\nContent-Type: application/json\r\n" % auth
    return head.encode() + b"\r\n" + (body or b"")


class Connection:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.step = 0
        self.post = 1

    def response(self):
        while True:
            end = self.buf.find(b"\r\n\r\n")
            if end >= 0:
                head = self.buf[:end]
                at = head.lower().find(b"content-length:")
                size = int(head[at + 15:].split(b"\r\n", 1)[0]) if at >= 0 else 0
                if len(self.buf) >= end + 4 + size:
                    body = self.buf[end + 4:end + 4 + size]
                    self.buf = self.buf[end + 4 + size:]
                    return int(head[9:12]), body
            more = self.sock.recv(65536)
            if not more:
                raise RuntimeError("the server closed a connection")
            self.buf += more


def one(port, raw):
    c = Connection(port)
    c.sock.sendall(raw)
    status, _ = c.response()
    c.sock.close()
    return status


def user_model(port, rnd, tokens):
    conns = [Connection(port) for _ in range(CONNECTIONS)]
    served = 0
    for _ in range(ROUNDS):
        waiting = []
        for c in conns:
            auth = "Bearer " + tokens[rnd.randrange(len(tokens))]
            if c.step == 0:
                raw, c.step = request("GET", "/feed"), 1
            elif c.step == 1:
                raw, c.step = request("GET", "/posts/%d" % c.post), 2
            else:
                c.step = 0
                roll = rnd.random()
                if roll < 0.15:
                    raw = request("POST", "/posts/%d/like" % c.post, auth)
                elif roll < 0.17:
                    text = "%s says hi at %s (VU %d)" % (rnd.choice(NOUNS), time.strftime("%Y-%m-%dT%H:%M:%S.000Z"), served)
                    raw = request("POST", "/posts", auth, json.dumps({"body": text}).encode())
                else:
                    continue
            c.sock.sendall(raw)
            waiting.append(c)
        for c in waiting:
            status, body = c.response()
            served += 1
            if status >= 500:
                raise RuntimeError("the server answered %d" % status)
            if c.step == 1:
                posts = json.loads(body)["posts"]
                c.post = posts[rnd.randrange(len(posts))]["id"]
    for c in conns:
        c.sock.close()
    return served


def error_paths(port):
    good = "Bearer " + token(7)
    now = int(time.time())
    cases = [
        (404, request("GET", "/nope")),
        (400, request("GET", "/posts/abc")),
        (400, request("GET", "/posts/0")),
        (404, request("GET", "/posts/999999999")),
        (200, request("GET", "/health")),
        (401, request("POST", "/posts", None, b'{"body":"x"}')),
        (401, request("POST", "/posts", "Basic abc", b'{"body":"x"}')),
        (401, request("POST", "/posts", "Bearer abc", b'{"body":"x"}')),
        (401, request("POST", "/posts", "Bearer " + token(7, exp=now - 5), b'{"body":"x"}')),
        (401, request("POST", "/posts", "Bearer " + sign({"sub": "x", "username": "u", "exp": now + 60}), b'{"body":"x"}')),
        (401, request("POST", "/posts", "Bearer " + sign({"sub": "7", "username": "u", "exp": now + 60}, alg="none"), b'{"body":"x"}')),
        (401, request("POST", "/posts/abc/like")),
        (400, request("POST", "/posts/abc/like", good)),
        (404, request("POST", "/posts/999999999/like", good)),
        (400, request("POST", "/posts", good, b'{"body":""}')),
        (400, request("POST", "/posts", good, b'{"body":123}')),
        (400, request("POST", "/posts", good, b'{bad json')),
        (400, request("POST", "/posts", good, json.dumps({"body": "a" * 501}).encode())),
        (201, request("POST", "/posts", good, '{"body":"  caf\\u00e9 \\"quoted\\" ✓ \\n "}'.encode())),
    ]
    for want, raw in cases:
        got = one(port, raw)
        if got != want:
            raise RuntimeError("expected %d, got %d for %r" % (want, got, raw[:60]))
    # two requests in one write, and one request in two
    c = Connection(port)
    c.sock.sendall(request("GET", "/feed") + request("GET", "/posts/1"))
    c.response(), c.response()
    raw = request("POST", "/posts/1/like", good)
    c.sock.sendall(raw[:40])
    time.sleep(0.05)
    c.sock.sendall(raw[40:])
    c.response()
    c.sock.close()


def main():
    server = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "train.db")
        make_database(path)
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        env = dict(os.environ, SQLITE_PATH=path, JWT_SECRET=SECRET.decode(), HOST="127.0.0.1", PORT=str(port))
        proc = subprocess.Popen(["bash", server], env=env)
        try:
            for _ in range(600):
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    if proc.poll() is not None:
                        raise RuntimeError("the server did not start")
                    time.sleep(0.05)
            rnd = random.Random(3)
            tokens = [token(rnd.randint(1, USERS)) for _ in range(400)]
            t0 = time.time()
            error_paths(port)
            served = user_model(port, rnd, tokens)
            print("trained: %d requests in %.1f s" % (served, time.time() - t0))
        finally:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=30)


if __name__ == "__main__":
    main()
