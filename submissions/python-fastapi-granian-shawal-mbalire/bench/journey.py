"""One user journey, in-process: the FastAPI app (real SQLite on the seed DB, real JWT) called directly,
without Granian or sockets. Prints µs per request for each endpoint and for the load mix.

For µs-level A/Bs the bench can't resolve. Run it inside a server image so the Python and SQLite match:

    docker run --rm -i --user root --cpuset-cpus 0 twelve-server-ours sh -c \
        'cp /home/runner/feed.db /feed.db && /app/.venv/bin/python - ' < bench/journey.py

Keep the Mac idle and a busy loop on another VM CPU (the clock keeper) while it runs. Compare reads
(feed, post): writes wait on the WAL write and vary 16-50 µs. Set PROFILE_HOPS_OUT=/tmp/hops.json to
time the timed runner instead of the untimed one.
"""
import asyncio, base64, hashlib, hmac, json, os, random, shutil, sqlite3, sys, time
shutil.copy("/feed.db", "/tmp/feed.db")
os.environ.update(SQLITE_PATH="/tmp/feed.db", JWT_SECRET="twelve-dollar-challenge")
sys.path.insert(0, "/app"); os.chdir("/app")
import main

def b64(raw): return base64.urlsafe_b64encode(raw).rstrip(b"=")
def token(uid):
    si = b64(b'{"alg":"HS256","typ":"JWT"}') + b"." + b64(json.dumps({"sub": str(uid), "username": f"user{uid}", "iat": 1, "exp": 4102444800}).encode())
    return b"Bearer " + si + b"." + b64(hmac.new(b"twelve-dollar-challenge", si, hashlib.sha256).digest())
async def send(message): pass
def receiver(body):
    async def receive(): return {"type": "http.request", "body": body, "more_body": False}
    return receive
def scope(method, path, auth=None):
    return {"type": "http", "method": method, "path": path, "root_path": "", "query_string": b"",
            "headers": [(b"authorization", auth)] if auth else []}

async def run():
    await main.store.start(); await main.budget.start()
    rng = random.Random(1)
    journeys = []
    for _ in range(12000):
        uid, pid = rng.randint(1, 20000), rng.randint(1, 500000)
        steps = [("feed", scope("GET", "/feed"), b""), ("post", scope("GET", f"/posts/{pid}"), b"")]
        if rng.random() < 0.15: steps.append(("like", scope("POST", f"/posts/{pid}/like", token(uid)), b""))
        if rng.random() < 0.02: steps.append(("create", scope("POST", "/posts", token(uid)), b'{"body":"hello there"}'))
        journeys.append(steps)
    app = main.app
    spent, count = {}, {}
    clock = time.perf_counter
    for rnd in range(4):  # round 0 warms up
        for steps in journeys:
            for kind, s, body in steps:
                t = clock()
                await app(dict(s), receiver(body), send)
                if rnd:
                    spent[kind] = spent.get(kind, 0.0) + clock() - t
                    count[kind] = count.get(kind, 0) + 1
            await asyncio.sleep(0)  # lets the group commit run, as the event loop would
    total_n = sum(count.values())
    parts = "  ".join(f"{k} {spent[k] / count[k] * 1e6:5.1f}" for k in ("feed", "post", "like", "create"))
    jit = getattr(getattr(sys, "_jit", None), "is_enabled", lambda: False)()
    print(f"{sys.version.split()[0]:8} jit={'on ' if jit else 'off'} sqlite {sqlite3.sqlite_version}  "
          f"mix {sum(spent.values()) / total_n * 1e6:5.2f} us/request  |  {parts}", flush=True)
asyncio.run(run())
