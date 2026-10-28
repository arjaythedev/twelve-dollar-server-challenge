#!/usr/bin/env python3
"""Exercise the supplied Nginx policy with private ports, logs and database."""
import gzip
import http.client
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]


def get(port, path="/health", headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        conn.request("GET", path, headers=headers or {})
        response = conn.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        conn.close()


def ready(process, port):
    started = time.monotonic()
    while time.monotonic() - started < 60:
        if process.poll() is not None:
            raise RuntimeError(f"process on port {port} exited: {process.returncode}")
        try:
            if get(port)[0] == 200:
                return time.monotonic() - started
        except OSError:
            time.sleep(0.02)
    raise TimeoutError(f"port {port} not healthy within 60 seconds")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        process.wait(timeout=10)


def main():
    with tempfile.TemporaryDirectory(prefix="ada-nginx-") as temp:
        directory = Path(temp)
        db = directory / "feed.db"
        shutil.copyfile(ROOT / "seed/feed.db", db)
        for part in ("client", "proxy", "fastcgi", "uwsgi", "scgi"):
            (directory / part).mkdir()
        # The shared configuration is read only. Only ports and log paths change
        # in this private copy; keepalive, proxy, gzip and logging policy remain.
        site = (ROOT / "bench/nginx.conf").read_text()
        site = site.replace("127.0.0.1:3000", "127.0.0.1:3001")
        site = site.replace("listen 80 default_server;", "listen 8081 default_server;")
        site = site.replace("/var/log/nginx", temp)
        config = directory / "nginx.conf"
        config.write_text(
            f"worker_processes 1;\npid {temp}/nginx.pid;\n"
            f"error_log {temp}/nginx.error.log warn;\n"
            "events { worker_connections 16384; }\n"
            "http {\n"
            f"client_body_temp_path {temp}/client;\nproxy_temp_path {temp}/proxy;\n"
            f"fastcgi_temp_path {temp}/fastcgi;\nuwsgi_temp_path {temp}/uwsgi;\n"
            f"scgi_temp_path {temp}/scgi;\n"
            + site + "\n}\n"
        )
        env = os.environ | {"SQLITE_PATH": str(db), "JWT_SECRET": "twelve-dollar-challenge",
                            "HOST": "127.0.0.1", "PORT": "3001"}
        app = nginx = None
        with open(directory / "server.log", "w+") as app_log, open(directory / "nginx.log", "w+") as nginx_log:
            try:
                started = time.monotonic()
                app = subprocess.Popen([str(HERE / "start.sh")], env=env, stdout=app_log, stderr=app_log)
                ready(app, 3001)
                print(f"App healthy after {time.monotonic() - started:.3f} seconds (local emulation)", flush=True)
                subprocess.run(["nginx", "-t", "-c", str(config), "-p", temp + "/"], check=True)
                nginx = subprocess.Popen(["nginx", "-c", str(config), "-p", temp + "/", "-g", "daemon off;"],
                                         stdout=nginx_log, stderr=nginx_log)
                ready(nginx, 8081)
                subprocess.run(["bash", str(ROOT / "test/test.sh"), "http://127.0.0.1:8081"], env=env, check=True)
                status, headers, compressed = get(8081, "/feed", {"Accept-Encoding": "gzip"})
                assert status == 200 and headers.get("Content-Encoding") == "gzip"
                status, _, direct = get(3001, "/feed")
                assert status == 200 and gzip.decompress(compressed) == direct
                assert len(json.loads(direct)["posts"]) == 20
                print(f"Nginx gzip roundtrip PASS: {len(direct)} bytes -> {len(compressed)} bytes", flush=True)
                print("Nginx integration PASS", flush=True)
            except BaseException:
                for log in (app_log, nginx_log):
                    log.flush()
                    log.seek(0)
                    print(log.read(), flush=True)
                raise
            finally:
                stop(nginx)
                stop(app)


if __name__ == "__main__":
    main()
