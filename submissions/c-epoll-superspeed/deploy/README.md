# Deploying on a 2 GB Ubuntu 24.04 VPS

These steps assume the challenge repository (or at least this submission
folder plus `seed/`) is on the server and that you are root.

## 1. Install the toolchain and build

```bash
cd submissions/c-epoll-superspeed
sudo bash install.sh          # build-essential, curl, unzip, ca-certificates
bash build.sh                 # downloads pinned SQLite 3.54.1, builds bin/server
```

`build.sh` uses `-march=x86-64-v2`, so the binary runs on any x86-64 server.
If you want to squeeze out a little more, rebuild with `MARCH=-march=native`
(or edit the script) on the exact machine that will serve traffic.

## 2. Build the seed database

On the server, from the challenge repo root:

```bash
sudo apt-get install -y nodejs sqlite3 jq
bash seed/make-seed.sh
```

It prints `OK: ... content sha256 matches`. The output is
`seed/feed.db` (~238 MB) and `seed/tokens.json` (for the load generator).
If the VPS is too small to build the seed comfortably, build it on another
machine and copy `feed.db` over; the `.db` file is self-contained.

## 3. Install as a systemd service

```bash
sudo useradd --system --home /opt/feed --shell /usr/sbin/nologin deploy || true
sudo mkdir -p /opt/feed/bin
sudo cp bin/server /opt/feed/bin/
sudo cp ../../seed/feed.db /opt/feed/feed.db
sudo chown -R deploy:deploy /opt/feed
sudo cp deploy/feed-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now feed-server
```

Check it:

```bash
systemctl status feed-server --no-pager
curl -s http://127.0.0.1/health
curl -s http://127.0.0.1/feed | head -c 200; echo
journalctl -u feed-server -n 20 --no-pager
```

The service runs 4 workers on `0.0.0.0:80` by default (see the unit file).
Each worker can hold about 60k connections under `LimitNOFILE=65535`, so four
workers give roughly 240k connection slots. If you put Nginx in front instead,
set `HOST=127.0.0.1 PORT=3000 WORKERS=1`.

## 4. Run the official test suite

The test suite must run against a **fresh** copy of the seed. It writes a post
and likes, so a second run on the same database fails the golden checks.

```bash
cd /path/to/twelve-dollar-server-challenge
bash test/run.sh submissions/c-epoll-superspeed
```

That is exactly what CI runs. It builds, starts on `127.0.0.1:3000` with a
temporary database copy, runs all 42 checks, and cleans up.

## 5. Load-test it

Run k6 from a **different machine** (the load generator should not compete with
the server for CPU). You need `seed/tokens.json` on the load machine.

```bash
# 1,000 virtual users, the official user model (feed -> think -> post -> ...)
k6 run -e BASE_URL=http://SERVER_IP -e VUS=1000 bench/load.js

# probe upwards; the score is the largest VUS that passes a 5-minute hold
k6 run -e BASE_URL=http://SERVER_IP -e VUS=10000 -e DURATION=5m bench/load.js
```

The pass thresholds are `p(95) < 500 ms`, `p(99) < 1000 ms`, and under 1%
failed requests. Every probe should start from a fresh database copy.

Useful knobs while tuning:

```bash
WORKERS=1|2|4   # process count; direct mode defaults to 4
DEBUG_CKPT=1    # log the checkpoint helper's nlog/nckpt once a second
```

Watch memory and the WAL:

```bash
watch -n1 'free -m; ls -lh /opt/feed/feed.db*; systemctl show feed-server -p MemoryCurrent'
```

The checkpoint helper should keep `feed.db-wal` at a few MB. If it grows to
tens of MB under load, check `DEBUG_CKPT` output: `nlog`/`nckpt` should be
close to each other and the helper should be running. The server's own
`wal_autocheckpoint` (50,000 pages) is only a safety net.

## 6. Resetting between benchmark runs

Stop the service, replace the database, and remove the old WAL/SHM files:

```bash
sudo systemctl stop feed-server
sudo rm -f /opt/feed/feed.db-wal /opt/feed/feed.db-shm
sudo cp /path/to/fresh/feed.db /opt/feed/feed.db
sudo chown deploy:deploy /opt/feed/feed.db
sudo systemctl start feed-server
```

Never copy over a live database: SQLite's WAL and shared-memory files must
match the database or you will see "database disk image is malformed".
