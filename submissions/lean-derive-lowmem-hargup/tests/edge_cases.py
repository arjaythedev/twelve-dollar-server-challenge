#!/usr/bin/env python3
"""Regression checks for fragmented/pipelined input and variable-size JSON output."""
import base64, hashlib, hmac, json, os, socket, time

HOST = os.environ.get("HOST", "127.0.0.1")
PORT = int(os.environ.get("PORT", "3000"))
SECRET = os.environ.get("JWT_SECRET", "twelve-dollar-challenge").encode()

def token():
    def b64(b):
        return base64.urlsafe_b64encode(b).rstrip(b'=')
    h=b64(b'{"alg":"HS256"}')
    p=b64(json.dumps({'sub':'1','username':'golden_ember_1','exp':int(time.time())+3600}).encode())
    msg=h+b'.'+p
    return msg+b'.'+b64(hmac.new(SECRET,msg,hashlib.sha256).digest())

class Client:
    def __init__(self):
        self.s=socket.create_connection((HOST,PORT),timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        self.data=b''
    def recv(self):
        while b'\r\n\r\n' not in self.data:
            part=self.s.recv(65536)
            assert part, 'unexpected EOF'
            self.data+=part
        h,body=self.data.split(b'\r\n\r\n',1)
        headers=dict(line.split(b':',1) for line in h.split(b'\r\n')[1:])
        n=int(headers[b'Content-Length'])
        while len(body)<n:
            part=self.s.recv(65536)
            assert part, 'truncated body'
            body+=part
        self.data=body[n:]
        assert headers[b'Content-Type'].strip().startswith(b'application/json')
        return int(h.split()[1]),json.loads(body[:n])
    def send(self,request,fragment=False):
        if fragment:
            # Split within the request line, header, and JSON body on separate receive rounds.
            cuts=sorted({3, min(40,len(request)-1),len(request)-7})
            start=0
            for end in cuts:
                self.s.sendall(request[start:end]); start=end; time.sleep(.002)
            self.s.sendall(request[start:])
        else:
            self.s.sendall(request)

def get(path):
    return b'GET '+path.encode()+b' HTTP/1.1\r\nHost: x\r\n\r\n'
def create(body):
    b=json.dumps({'body':body},ensure_ascii=False,separators=(',',':')).encode()
    return b'POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer '+TOKEN+b'\r\nContent-Length: '+str(len(b)).encode()+b'\r\n\r\n'+b

TOKEN=token()
c=Client()
patterns=['a'*500,'"'*500,'\\'*500,'X'+''.join(chr(i) for i in range(32))*15+'Z',
          'é'*500,'界'*500,'🙂'*200,'short']
created={}
for i in range(32):
    body=patterns[i%len(patterns)]
    # A write followed by a pipelined feed must expose the committed post.
    c.send(create(body)+get('/feed')+get('/health'),fragment=True)
    status,data=c.recv()
    assert status==201,(status,data)
    post=data['post']
    assert post['body']==body and post['like_count']==0
    assert list(post)==['id','body','created_at','author','like_count']
    created[post['id']]=body
    status,data=c.recv()
    assert status==200 and data['posts'][0]['id']==post['id']
    for p in data['posts']:
        if p['id'] in created:
            assert p['body']==created[p['id']]
    assert c.recv()[0]==200
    c.send(get('/posts/'+str(post['id'])),fragment=True)
    assert c.recv()==(200,{'post':post})
# Exercise response boundaries when several reads are parsed together.
c.send(get('/health')*100)
for _ in range(100):
    assert c.recv()[0]==200
c.s.close()
print('edge cases: 32 fragmented write/read pipelines, escaping, long UTF-8, and 100 pipelined reads passed')
