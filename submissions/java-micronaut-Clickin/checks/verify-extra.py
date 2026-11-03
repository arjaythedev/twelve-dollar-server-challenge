#!/usr/bin/env python3
"""Black-box checks that supplement the unchanged official suite."""
import base64
import concurrent.futures
import hashlib
import hmac
import json
import os
import time
import urllib.error
import urllib.request

BASE = os.environ.get('BASE_URL', 'http://127.0.0.1:3000')
SECRET = os.environ.get('JWT_SECRET', 'twelve-dollar-challenge').encode()

def token(sub='1', username='golden_ember_1'):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b'=')
    data = encode({'alg': 'HS256'}) + b'.' + encode({'sub': sub, 'username': username, 'exp': int(time.time()) + 3600})
    return (data + b'.' + base64.urlsafe_b64encode(hmac.digest(SECRET, data, 'sha256')).rstrip(b'=')).decode()

def request(method, path, data=None, jwt=None):
    headers = {'Content-Type': 'application/json'}
    if jwt is not None:
        headers['Authorization'] = 'Bearer ' + jwt
    raw = data.encode() if isinstance(data, str) else None
    req = urllib.request.Request(BASE + path, raw, headers, method=method)
    try:
        response = urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.loads(response.read())

jwt = token()
assert request('POST', '/posts', '{bad') == (401, {'error': 'missing bearer token'})
for value in ['null', '[]', '{"body":false}', '{"body":null}']:
    assert request('POST', '/posts', value, jwt) == (400, {'error': 'body is required'})
assert request('POST', '/posts', '{"body":"x"}', token(1))[0] == 401
assert request('POST', '/posts', '{"body":"x"}', token(username=1))[0] == 401
assert request('POST', '/posts', '{"body":"must not persist"}', token(sub='999999999'))[0] == 500
status, created = request('POST', '/posts', json.dumps({'body': '😀' * 500}), jwt)
assert status == 201, (status, created)
post_id = created['post']['id']
assert list(created['post']) == ['id', 'body', 'created_at', 'author', 'like_count']
assert request('GET', f'/posts/{post_id}')[1]['post']['body'] == '😀' * 500
with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
    likes = list(pool.map(lambda _: request('POST', f'/posts/{post_id}/like', jwt=jwt), range(32)))
assert sum(status == 201 for status, _ in likes) == 1, likes
assert sum(status == 200 for status, _ in likes) == 31, likes
assert request('GET', f'/posts/{post_id}')[1]['post']['like_count'] == 1
with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
    bodies = [f'concurrent-{i}: "quoted" \\ slash\nnewline\ttab\x00nul 한글 😀' for i in range(32)]
    posts = list(pool.map(lambda body: request('POST', '/posts', json.dumps({'body': body}), jwt), bodies))
assert all(status == 201 for status, _ in posts)
ids = [body['post']['id'] for _, body in posts]
assert len(set(ids)) == 32
for (_, result), body in zip(posts, bodies):
    assert result['post']['body'] == body
    assert request('GET', f"/posts/{result['post']['id']}") == (200, result)
expected_feed = sorted((result['post'] for _, result in posts),
                       key=lambda post: (post['created_at'], post['id']), reverse=True)[:20]
assert request('GET', '/feed') == (200, {'posts': expected_feed})
reads = [(f"/posts/{result['post']['id']}", (200, result)) for _, result in posts]
reads.append(('/posts/999999999', (404, {'error': 'post not found'})))
with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
    actual = list(pool.map(lambda item: request('GET', item[0]), reads * 4))
assert actual == [expected for _, expected in reads * 4]
print('PASS: auth precedence, JSON types, Unicode boundary, field order, concurrent likes, committed write visibility, escaped JSON round-trip, feed ordering, SQL failure recovery and concurrent parameter rebinding')
