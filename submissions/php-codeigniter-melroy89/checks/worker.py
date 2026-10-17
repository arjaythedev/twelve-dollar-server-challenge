#!/usr/bin/env python3
"""Additional worker-isolation/concurrency checks. Run ONLY on a disposable DB copy."""
import base64
import concurrent.futures
import hashlib
import hmac
import http.client
import json
import os
import sys
import time
from urllib.parse import urlsplit

base = urlsplit(sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:3000')
secret = os.environ.get('JWT_SECRET', 'twelve-dollar-challenge').encode()

def token(sub='1', username='golden_ember_1', **claims):
    encode = lambda data: base64.urlsafe_b64encode(json.dumps(data, separators=(',', ':')).encode()).rstrip(b'=')
    header = encode({'alg': 'HS256', 'typ': 'JWT'})
    payload = encode({'sub': sub, 'username': username, 'exp': int(time.time()) + 3600, **claims})
    signed = header + b'.' + payload
    return (signed + b'.' + base64.urlsafe_b64encode(hmac.digest(secret, signed, 'sha256')).rstrip(b'=')).decode()

def req(method, path, auth=None, data=None, connection=None):
    conn = connection or http.client.HTTPConnection(base.hostname, base.port or 80, timeout=30)
    headers = {'Content-Type': 'application/json'}
    if auth is not None:
        headers['Authorization'] = 'Bearer ' + auth
    conn.request(method, path, body=json.dumps(data, ensure_ascii=False).encode() if data is not None else None, headers=headers)
    response = conn.getresponse()
    result = response.status, json.loads(response.read())
    if connection is None:
        conn.close()
    return result

# Reuse the same client connection across different identities and invalid/absent auth.
conn = http.client.HTTPConnection(base.hostname, base.port or 80, timeout=30)
for user in range(1, 21):
    name = f'claimed-user-{user}'
    status, result = req('POST', '/posts', token(str(user), name), {'body': f'worker isolation {user}'}, conn)
    assert status == 201 and result['post']['author'] == name, result
    status, result = req('POST', '/posts', data={'body': 'must fail'}, connection=conn)
    assert (status, result) == (401, {'error': 'missing bearer token'}), (status, result)
    status, result = req('POST', '/posts', token(str(user), name, exp=0), {'body': 'expired'}, conn)
    assert status == 401 and result == {'error': 'invalid or expired token'}, result
conn.close()

for claims, message in [({'sub': 1}, 'invalid token payload'), ({'username': 2}, 'invalid token payload'),
                        ({'exp': '9999999999'}, 'invalid or expired token'), ({'exp': None}, 'invalid or expired token'),
                        ({'nbf': int(time.time()) + 3600}, 'invalid or expired token')]:
    assert req('POST', '/posts', token(**claims), {'body': 'reject'}) == (401, {'error': message})

status, result = req('POST', '/posts', token(), {'body': '✓' * 500})
assert status == 201 and result['post']['body'] == '✓' * 500, result
post_id = result['post']['id']
assert req('POST', '/posts', token(), {'body': '✓' * 501}) == (400, {'error': 'body must be at most 500 characters'})

with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
    results = list(pool.map(lambda _: req('POST', f'/posts/{post_id}/like', token()), range(64)))
assert sum(status == 201 for status, _ in results) == 1, results
assert sum(status == 200 for status, _ in results) == 63, results
for status, result in results:
    assert result == {'liked': True, 'already_liked': status == 200, 'post_id': post_id}, result

# Repeated reads must see committed writes made through either PHP worker.
for _ in range(64):
    status, result = req('GET', f'/posts/{post_id}')
    assert status == 200 and result['post']['like_count'] == 1, result

# Failed FK writes must not poison a reused prepared statement/connection.
assert req('POST', '/posts', token('999999999'), {'body': 'bad FK'}) == (500, {'error': 'internal server error'})
assert req('POST', '/posts', token(), {'body': 'after failed write'})[0] == 201
print('PASS: worker isolation, fresh JWT checks, Unicode limits, 64 concurrent duplicate likes, cross-worker visibility, FK failure recovery')
