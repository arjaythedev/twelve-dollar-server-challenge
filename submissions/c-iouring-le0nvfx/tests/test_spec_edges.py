#!/usr/bin/env python3
"""Regression checks for valid inputs beyond the challenge's golden cases."""
import argparse
import base64
import contextlib
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

p=argparse.ArgumentParser()
p.add_argument('--start',type=Path,required=True)
p.add_argument('--seed',type=Path,required=True)
a=p.parse_args()
with socket.socket() as s:
    s.bind(('127.0.0.1',0))
    port=s.getsockname()[1]
base=f'http://127.0.0.1:{port}'
secret=b'twelve-dollar-challenge'
def token(username='golden_ember_1', exp_json=None, raw_payload=None):
    enc=lambda b:base64.urlsafe_b64encode(b).rstrip(b'=')
    now=int(time.time())
    header=enc(b'{"alg":"HS256","typ":"JWT"}')
    payload_json=json.dumps({'sub':'1','username':username,'iat':now,'exp':now+3600},separators=(',',':')).encode()
    if exp_json is not None:
        expected=b'\"exp\":'+str(now+3600).encode()
        if expected not in payload_json: raise AssertionError('could not set numeric expiration')
        payload_json=payload_json.replace(expected,b'\"exp\":'+exp_json.encode(),1)
    if raw_payload is not None: payload_json=raw_payload
    payload=enc(payload_json)
    message=header+b'.'+payload
    return (message+b'.'+enc(hmac.new(secret,message,hashlib.sha256).digest())).decode()
def request(payload, username='golden_ember_1', exp_json=None, raw_payload=None):
    req=urllib.request.Request(base+'/posts',data=payload,headers={'Content-Type':'application/json','Authorization':'Bearer '+token(username,exp_json,raw_payload)})
    try: response=urllib.request.urlopen(req,timeout=15)
    except urllib.error.HTTPError as error: response=error
    with response:
        return response.status,json.load(response)
def encode(obj): return json.dumps(obj,ensure_ascii=False,separators=(',',':')).encode()
def expect(payload,status,*,body=None,error=None,username='golden_ember_1',exp_json=None,raw_payload=None):
    actual,result=request(payload,username,exp_json,raw_payload)
    if actual!=status: raise AssertionError(f'expected {status}, got {actual}: {result}')
    if error is not None and result.get('error')!=error: raise AssertionError(result)
    if body is not None and (result['post']['body']!=body or result['post']['author']!=username):
        raise AssertionError('body or token username was not preserved')
    return result
with tempfile.TemporaryDirectory(prefix='12d-spec-edges-') as tmp:
    db=Path(tmp)/'feed.db'; shutil.copyfile(a.seed.resolve(),db)
    env=os.environ.copy(); env.update(HOST='127.0.0.1',PORT=str(port),SQLITE_PATH=str(db),JWT_SECRET=secret.decode())
    with (Path(tmp)/'server.log').open('wb') as log:
        proc=subprocess.Popen([str(a.start.resolve())],env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+60
            while time.monotonic()<deadline:
                if proc.poll() is not None:
                    log.flush(); raise RuntimeError(Path(log.name).read_text())
                try:
                    with urllib.request.urlopen(base+'/health',timeout=1): break
                except (OSError,urllib.error.URLError): time.sleep(.1)
            else: raise RuntimeError('server did not start')
            for payload in (b'null', b'[]', b'12', b'\"text\"'):
                expect(payload,400,error='body is required')
            expect(encode({'body':' '*16000+'x'}),201,body='x')
            exactly_16k=b'{"body":"'+b' '*16372+b'x"}'
            assert len(exactly_16k)==16384
            expect(exactly_16k,201,body='x')
            expect(encode({'body':'x'*5000}),400,error='body must be at most 500 characters')
            expect(encode({'body':'\u00a0\u3000'}),400,error='body is required')
            expect(encode({'body':'\u00a0 x \u3000'}),201,body='x')
            expect(encode({'body':'😀'*500}),201,body='😀'*500)
            expect(encode({'body':'😀'*501}),400,error='body must be at most 500 characters')
            expect(encode({'body\x00suffix':'x'}),400,error='body is required')
            obj={f'extra_{i}':i for i in range(100)}; obj['body']='extra keys'
            expect(encode(obj),201,body='extra keys')
            expect(encode({'long-key-'+'x'*1000:0,'body':'long key'}),201,body='long key')
            expect(b'{"body":"old","body":"new"}',201,body='new')
            expect(b'{"extra":'+b'['*8000+b'0'+b']'*8000+b',"body":"deep"}',201,body='deep')
            expect(b'{"extra":[1,],"body":"x"}',400,error='malformed JSON body')
            expect(b'{"extra":{"a":1,},"body":"x"}',400,error='malformed JSON body')
            expect(b'{"body":"\xff"}',400,error='malformed JSON body')
            username='a'*1800+'\x00tail'
            expect(encode({'body':'author'}),201,body='author',username=username)
            post=expect(encode({'body':'a\x00b'}),201,body='a\x00b')
            with urllib.request.urlopen(base+'/posts/'+str(post['post']['id']),timeout=15) as response:
                if json.load(response)['post']['body']!='a\x00b':
                    raise AssertionError('embedded NUL body was not preserved on read')
            expect(encode({'body':'fractional expiration'}),201,body='fractional expiration',exp_json='4102444800.5')
            expect(encode({'body':'exponent expiration'}),201,body='exponent expiration',exp_json='4.1024448e9')
            expect(encode({'body':'expired'}),401,error='invalid or expired token',exp_json='0.5')
            expect(encode({'body':'malformed token'}),401,error='invalid or expired token',raw_payload=b'{')
            large_body='\x01'*500
            for _ in range(20):
                expect(encode({'body':large_body}),201,body=large_body)
            with urllib.request.urlopen(base+'/feed',timeout=15) as response:
                feed_raw=response.read()
            feed=json.loads(feed_raw)
            if len(feed_raw)<=16384 or len(feed['posts'])!=20 or any(
                post['body']!=large_body for post in feed['posts']):
                raise AssertionError('large feed was truncated or incorrect')
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=10)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    with contextlib.closing(sqlite3.connect(db)) as conn:
        assert conn.execute('pragma integrity_check').fetchone()[0]=='ok'
        assert conn.execute('pragma foreign_key_check').fetchone() is None
print('PASS: long bodies, Unicode, UTF-8, JSON keys, JWT usernames and large feeds')
