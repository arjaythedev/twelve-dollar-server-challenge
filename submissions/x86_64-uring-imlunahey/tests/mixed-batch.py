#!/usr/bin/env python3
"""Freeze mixed valid/FK-invalid writes plus reads before owner processing."""
import base64,hashlib,hmac,json,os,pathlib,shutil,signal,socket,sqlite3,subprocess,tempfile,time,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
secret=b'mixed-test'
def jwt(sub):
    enc=lambda obj:base64.urlsafe_b64encode(json.dumps(obj).encode()).rstrip(b'=')
    raw=enc({'alg':'HS256'})+b'.'+enc({'sub':str(sub),'username':'mixed','exp':int(time.time())+600})
    return raw+b'.'+base64.urlsafe_b64encode(hmac.digest(secret,raw,'sha256')).rstrip(b'=')
def response(s):
    f=s.makefile('rb');status=int(f.readline().split()[1]);headers={}
    while True:
        line=f.readline()
        if line==b'\r\n':break
        k,v=line.split(b':',1);headers[k.lower()]=v.strip()
    result=json.loads(f.read(int(headers[b'content-length'])));f.close();s.close()
    return status,result
for mode in ['writes','batch']:
    with tempfile.TemporaryDirectory() as tmp:
        db=pathlib.Path(tmp)/'feed.db';shutil.copyfile(ROOT.parents[1]/'seed/feed.db',db)
        env=os.environ|dict(SQLITE_PATH=str(db),JWT_SECRET=secret.decode(),HOST='127.0.0.1',PORT='3198')
        with open(pathlib.Path(tmp)/'log','w') as log:
            p=subprocess.Popen([str(ROOT/'bin/server'),f'--transaction={mode}','--savepoints=off'],env=env,stdout=log,stderr=log)
            sockets=[]
            try:
                for _ in range(100):
                    try:
                        with urllib.request.urlopen('http://127.0.0.1:3198/health',timeout=.1):break
                    except OSError:time.sleep(.02)
                # Let accepts and receives arm, then queue one frozen burst.
                sockets=[socket.create_connection(('127.0.0.1',3198),timeout=5) for _ in range(96)]
                time.sleep(.1);p.send_signal(signal.SIGSTOP)
                for i,s in enumerate(sockets):
                    if i%3==2:
                        req=b'GET /feed HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
                    else:
                        body=json.dumps({'body':f'mixed-case-{i}'}).encode()
                        req=b'POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer '+jwt(1 if i%3==0 else 999999999)+b'\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body
                    s.sendall(req)
                p.send_signal(signal.SIGCONT)
                results=[response(s) for s in sockets];sockets=[]
                for i,(status,data) in enumerate(results):
                    assert status==(201 if i%3==0 else 500 if i%3==1 else 200),(mode,i,status,data)
                p.kill();p.wait(5)
                with sqlite3.connect(db) as c:
                    for i,(status,data) in enumerate(results):
                        if i%3==0:assert c.execute('SELECT body FROM posts WHERE id=?',(data['post']['id'],)).fetchone()==(f'mixed-case-{i}',)
                        if i%3==2:
                            for post in data['posts']:
                                assert c.execute('SELECT body FROM posts WHERE id=?',(post['id'],)).fetchone()==(post['body'],)
                c.close()
                print('PASS',mode,'32 valid + 32 FK-invalid + 32 dependent reads',flush=True)
            finally:
                for s in sockets:s.close()
                if p.poll() is None:p.send_signal(signal.SIGCONT);p.terminate();p.wait(5)
