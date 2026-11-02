#!/usr/bin/env python3
"""Require responses to every POST after creating more idle clients than reuse slots."""
import sys,os,time,tempfile,subprocess,shutil,http.client,collections,json,concurrent.futures
from pathlib import Path
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root/'submissions/java-jdkhttpserver-razorree/tests'))
from check import token
out=Path(sys.argv[1]) if len(sys.argv)>1 else root/'results/jdkhttpserver-announced-close-20261101'
out.mkdir(parents=True,exist_ok=True)
conns=[]
with tempfile.TemporaryDirectory() as tmp, (out/'server.log').open('w') as log:
 db=Path(tmp)/'feed.db';shutil.copyfile(root/'seed/feed.db',db)
 env=dict(os.environ,SQLITE_PATH=str(db),JWT_SECRET='twelve-dollar-challenge',HOST='127.0.0.1',PORT='3224',JAVA_HOME=os.environ.get('JAVA_HOME',str(Path.home()/'.sdkman/candidates/java/current')))
 server=subprocess.Popen(['bash',str(root/'submissions/java-jdkhttpserver-razorree/start.sh')],env=env,stdout=log,stderr=log)
 try:
  time.sleep(2)
  for i in range(9500):
   c=http.client.HTTPConnection('127.0.0.1',3224,timeout=5)
   c.request('GET','/health');r=c.getresponse();r.read()
   if r.status!=200:raise RuntimeError(r.status)
   conns.append(c)
  headers={'Authorization':'Bearer '+token(),'Content-Length':'0'}
  def check(pair):
   index,c=pair
   try:
    if index % 2:
     c.request('POST','/posts',body=b'{"body":"connection overflow regression"}',headers={'Authorization': headers['Authorization'], 'Content-Type':'application/json'})
    else:
     c.request('POST','/posts/1/like',headers=headers)
    r=c.getresponse();r.read();return str(r.status)
   except Exception as e:return type(e).__name__
  with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
   outcomes=collections.Counter(ex.map(check,enumerate(conns)))
  for i in range(30):
   conns[0].request('GET','/health'); response=conns[0].getresponse(); response.read()
   assert (response.getheader('Connection') == 'close') == (i == 29), (i,response.getheaders())
  assert conns[0].sock is None
  result={'connections':9500,'idle_limit':8192,'post_reuse_outcomes':dict(outcomes),'expected_excess':9500-8192}
  assert outcomes.get('RemoteDisconnected',0)==0 and sum(outcomes.get(k,0) for k in ('200','201'))==9500, outcomes
  (out/'idle-reuse.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
 finally:
  for c in conns:c.close()
  server.terminate()
  try:server.wait(10)
  except subprocess.TimeoutExpired:server.kill();server.wait()
