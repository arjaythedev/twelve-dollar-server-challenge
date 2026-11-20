#!/usr/bin/env python3
"""Validation must survive endpoint-specific claim materialization."""
import base64, hashlib, hmac, http.client, json, os, time
key=os.environ.get('JWT_SECRET','twelve-dollar-challenge').encode()
def b64(x): return base64.urlsafe_b64encode(x).rstrip(b'=')
def jwt(payload,alg='HS256'):
    msg=b64(json.dumps({'alg':alg}).encode())+b'.'+b64(json.dumps(payload,ensure_ascii=True).encode())
    return (msg+b'.'+b64(hmac.new(key,msg,hashlib.sha256).digest())).decode()
def call(path,token,body=None):
    c=http.client.HTTPConnection(os.environ.get('HOST','127.0.0.1'),int(os.environ.get('PORT','3000')),timeout=15)
    headers={'Content-Type':'application/json'}
    if token is not None: headers['Authorization']='Bearer '+token
    c.request('POST',path,body=None if body is None else json.dumps(body),headers=headers)
    r=c.getresponse(); value=json.loads(r.read()); status=r.status; c.close()
    return status,value
base={'sub':'1','username':'quoted " name \\ ☺ 🙂','exp':int(time.time())+3600}
cases=[('username missing',{'sub':'1','exp':base['exp']},'invalid token payload'),
       ('username wrong type',dict(base,username=123),'invalid token payload'),
       ('sub wrong type',dict(base,sub=1),'invalid token payload'),
       ('sub invalid',dict(base,sub='abc'),'invalid token payload'),
       ('username raw limit',dict(base,username='x'*513),'invalid token payload'),
       ('escaped raw limit',dict(base,username='漢'*100),'invalid token payload'),
       ('expired',dict(base,exp=0),'invalid or expired token')]
for label,payload,error in cases:
    for path,body in [('/posts/1/like',None),('/posts',{'body':'claim check'})]:
        assert call(path,jwt(payload),body)==(401,{'error':error}),label
for tok in [jwt(base,alg='HS512'),jwt(base)[:-2]+'xx']:
    assert call('/posts/1/like',tok)==(401,{'error':'invalid or expired token'})
assert call('/posts/abc/like',None)==(401,{'error':'missing bearer token'})
assert call('/posts/abc/like',jwt(base))==(400,{'error':'invalid post id'})
status,value=call('/posts',jwt(base),{'body':'claim projection keeps the creation author'})
assert status==201 and value['post']['author']==base['username'],(status,value)
status,value=call('/posts/1/like',jwt(base))
assert status in (200,201) and value['liked'] is True
for name in ['', 'x'*512, '"\\'*120]:
    payload=dict(base,username=name)
    status,value=call('/posts',jwt(payload),{'body':'direct claim ownership'})
    assert status==201 and value['post']['author']==name
print('claim cases: validation/error order and escaped creation username passed')
