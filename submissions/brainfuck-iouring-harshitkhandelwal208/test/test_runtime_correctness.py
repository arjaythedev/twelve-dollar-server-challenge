#!/usr/bin/env python3
"""
Comprehensive HTTP Runtime Correctness Test Suite.
Validates:
1. TCP Fragmentation (1-byte chunked headers and bodies with sleep delays).
2. Content-Length handling and request body accumulation.
3. HTTP/1.1 keep-alive and connection reuse.
4. HTTP pipelining (multiple requests packed in one TCP send).
5. Brainfuck tape state and request context isolation across requests.
6. Malformed requests and clean connection closure.
"""

import socket
import time
import subprocess
import os
import sys
import hmac
import hashlib
import base64
import json
import threading

PORT = 19875
DB_PATH = "/tmp/feed_test_runtime.db"
SEED_DB = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "seed", "feed.db"))
SERVER_BIN = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "bin", "server"))

def make_jwt(sub=1, username="alice"):
    header = base64.urlsafe_b64encode(b'{"alg":"HS256","typ":"JWT"}').rstrip(b'=')
    payload_data = {"sub": sub, "username": username, "exp": int(time.time()) + 7200}
    payload = base64.urlsafe_b64encode(json.dumps(payload_data, separators=(',', ':')).encode()).rstrip(b'=')
    signing_input = header + b'.' + payload
    sig = base64.urlsafe_b64encode(hmac.new(b'twelve-dollar-challenge', signing_input, hashlib.sha256).digest()).rstrip(b'=')
    return (signing_input + b'.' + sig).decode()

def run_tests():
    # Setup test db
    if os.path.exists("/tmp/feed_seed.db"):
        subprocess.run(["cp", "/tmp/feed_seed.db", DB_PATH], check=True)
    elif os.path.exists(SEED_DB):
        subprocess.run(["cp", SEED_DB, DB_PATH], check=True)
    else:
        # Fallback to local feed.db if present
        local_db = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "feed.db"))
        subprocess.run(["cp", local_db, DB_PATH], check=True)

    env = os.environ.copy()
    env["PORT"] = str(PORT)
    env["SQLITE_PATH"] = DB_PATH

    print(f"Starting server binary: {SERVER_BIN} on port {PORT}...")
    server_proc = subprocess.Popen([SERVER_BIN], env=env)
    time.sleep(1.0)

    try:
        token = make_jwt()

        # Test 1: Basic GET /health
        print("  [Test 1] Standard GET /health...", end=" ", flush=True)
        s = socket.create_connection(("127.0.0.1", PORT))
        s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        resp = s.recv(4096).decode(errors="ignore")
        s.close()
        assert "200 OK" in resp and "ok" in resp
        print("PASS")

        # Test 2: Extreme TCP Fragmentation (headers sent byte-by-byte)
        print("  [Test 2] Extreme TCP Fragmentation (headers byte-by-byte)...", end=" ", flush=True)
        s = socket.create_connection(("127.0.0.1", PORT))
        req = b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        for i in range(len(req)):
            s.sendall(req[i:i+1])
            time.sleep(0.001)
        resp = b""
        while True:
            chunk = s.recv(4096)
            if not chunk: break
            resp += chunk
        s.close()
        assert b"200 OK" in resp
        print("PASS")

        # Test 3: Fragmented POST with Content-Length (header split, body chunked)
        print("  [Test 3] Fragmented POST /posts (body chunked across packets)...", end=" ", flush=True)
        s = socket.create_connection(("127.0.0.1", PORT))
        body = b'{"body":"Fragmented hello world"}'
        headers = (
            f"POST /posts HTTP/1.1\r\n"
            f"Host: localhost\r\n"
            f"Authorization: Bearer {token}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n\r\n"
        ).encode()

        # Send headers in two parts
        half = len(headers) // 2
        s.sendall(headers[:half])
        time.sleep(0.02)
        s.sendall(headers[half:])
        time.sleep(0.02)
        # Send body in 3-byte chunks
        for i in range(0, len(body), 3):
            s.sendall(body[i:i+3])
            time.sleep(0.005)

        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = s.recv(4096)
            if not chunk: break
            resp += chunk
        s.close()
        print("Test 3 resp:", repr(resp))
        assert b"201 Created" in resp
        print("PASS")

        # Test 4: HTTP/1.1 Pipelining (multiple requests in single sendall)
        print("  [Test 4] HTTP Pipelining (2 requests in single TCP packet)...", end=" ", flush=True)
        s = socket.create_connection(("127.0.0.1", PORT))
        pipelined = (
            b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
            b"GET /posts/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        )
        s.sendall(pipelined)
        resp = b""
        s.settimeout(3.0)
        while True:
            try:
                chunk = s.recv(4096)
                if not chunk: break
                resp += chunk
            except socket.timeout:
                break
        s.close()
        # Should contain two 200 OK responses
        assert resp.count(b"200 OK") == 2
        print("PASS")

        # Test 5: Tape State Isolation across alternating requests
        print("  [Test 5] Brainfuck tape state isolation...", end=" ", flush=True)
        s = socket.create_connection(("127.0.0.1", PORT))
        for _ in range(5):
            # Request 1: GET /health
            s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
            r1 = s.recv(4096)
            assert b"200 OK" in r1 and b"ok" in r1

            # Request 2: GET /posts/1
            s.sendall(b"GET /posts/1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
            r2 = s.recv(4096)
            assert b"200 OK" in r2 and b"id" in r2

            # Request 3: GET nonexistent -> 404
            s.sendall(b"GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n")
            r3 = s.recv(4096)
            assert b"404 Not Found" in r3
        s.close()
        print("PASS")

        # Test 6: Malformed request handling & rejection verification
        print("  [Test 6] Malformed request handling & rejection verification...", end=" ", flush=True)
        malformed_inputs = [
            b"NOT_A_VALID_HTTP_REQUEST\r\n\r\n",
            b"GET /path_without_version\r\n\r\n",
            b"INVALID_METHOD /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
            b"\r\n\r\n",
        ]
        for bad_req in malformed_inputs:
            s = socket.create_connection(("127.0.0.1", PORT))
            s.sendall(bad_req)
            r = b""
            try:
                s.settimeout(1.0)
                r = s.recv(4096)
            except socket.timeout:
                pass
            s.close()
            # Server must either close connection cleanly (EOF: b"") or return 40x error
            assert (r == b"" or b"HTTP/1.1 40" in r), f"Expected connection close or 40x error for {bad_req!r}, got {r!r}"

        # Verify that after handling malformed inputs, the server remains healthy
        s = socket.create_connection(("127.0.0.1", PORT))
        s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        r = s.recv(4096)
        s.close()
        assert b"200 OK" in r and b"\"status\":\"ok\"" in r, f"Server failed health check after malformed inputs: {r!r}"
        print("PASS")

        # Test 7: Simultaneous multi-client concurrency (50 parallel clients)
        print("  [Test 7] Simultaneous multi-client concurrency (50 parallel clients)...", end=" ", flush=True)
        num_clients = 50
        barrier = threading.Barrier(num_clients)
        client_results = [None] * num_clients

        def client_worker(idx):
            try:
                s = socket.create_connection(("127.0.0.1", PORT), timeout=5.0)
                barrier.wait(timeout=5.0)

                if idx % 4 == 0:
                    req = b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                    expected_status = b"200 OK"
                elif idx % 4 == 1:
                    req = b"GET /feed HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                    expected_status = b"200 OK"
                elif idx % 4 == 2:
                    req = b"GET /posts/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
                    expected_status = b"200 OK"
                else:
                    post_body = f'{{"body":"Simultaneous client {idx}"}}'.encode()
                    req = (
                        f"POST /posts HTTP/1.1\r\n"
                        f"Host: localhost\r\n"
                        f"Authorization: Bearer {token}\r\n"
                        f"Content-Type: application/json\r\n"
                        f"Content-Length: {len(post_body)}\r\n"
                        f"Connection: close\r\n\r\n"
                    ).encode() + post_body
                    expected_status = b"201 Created"

                s.sendall(req)
                resp = b""
                while True:
                    chunk = s.recv(4096)
                    if not chunk: break
                    resp += chunk
                s.close()

                if expected_status in resp:
                    client_results[idx] = "OK"
                else:
                    client_results[idx] = f"Status mismatch: expected {expected_status}, got {resp[:100]!r}"
            except Exception as e:
                client_results[idx] = f"Exception: {e}"

        threads = [threading.Thread(target=client_worker, args=(i,)) for i in range(num_clients)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10.0)

        failures = [res for res in client_results if res != "OK"]
        assert len(failures) == 0, f"{len(failures)} simultaneous clients failed: {failures[:5]}"
        print(f"PASS (all {num_clients} concurrent requests succeeded)")

        print("\nAll HTTP runtime correctness tests PASSED!")
        return True

    finally:
        server_proc.terminate()
        server_proc.wait()
        if os.path.exists(DB_PATH):
            os.remove(DB_PATH)

if __name__ == "__main__":
    if not run_tests():
        sys.exit(1)
