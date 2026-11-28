import json, os, shutil, sqlite3, subprocess, sys
from contextlib import closing
from pathlib import Path
B=Path(sys.argv[1]); R=Path(sys.argv[2]); D=B/'cases'; D.mkdir(exist_ok=True)
def fresh(name):
 p=D/(name+'.db'); p.unlink(missing_ok=True)
 for x in ('-wal','-shm'): Path(str(p)+x).unlink(missing_ok=True)
 shutil.copyfile(R/'seed/feed.db',p); return p
def run(db, mode='check', extra=None, binary='full'):
 env=os.environ.copy(); env.update(extra or {})
 r=subprocess.run([str(B/binary),str(db),mode],env=env,text=True,capture_output=True)
 assert r.returncode==0,(r.returncode,r.stderr,extra)
 rows={}
 for line in r.stdout.splitlines():
  k,_,v=line.partition(' '); rows.setdefault(k,[]).append(v)
 return rows
def expected(db):
 with closing(sqlite3.connect(db)) as c, c:
  q='SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON p.user_id=u.id ORDER BY p.created_at DESC,p.id DESC LIMIT 20'
  return [dict(zip(('id','body','created_at','author','like_count'),x)) for x in c.execute(q)]
def verify(db, rows):
 for key in ('feed','after_feed','moved_feed'):
  if key in rows: assert json.loads(rows[key][-1])['posts']==expected(db),(key,db)
 for key in ('post','before_commit','after_commit','after_lowlike'):
  for row in rows.get(key,[]):
   p=json.loads(row)['post']
   with closing(sqlite3.connect(db)) as c, c:
    ref=c.execute('SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON p.user_id=u.id WHERE p.id=?',(p['id'],)).fetchone()
   assert p==dict(zip(('id','body','created_at','author','like_count'),ref)),(key,p,ref)
checks=0
p=fresh('normal')
full=run(p); verify(p,full); checks+=1
verify(p,run(p,'counts')); checks+=1
for extra in ({'NO_AUTHORS':'1'},{'NO_TABLE':'1'},{'NO_AUTHORS':'1','NO_TABLE':'1'}):
 rows=run(p,extra=extra); assert rows==full; verify(p,rows); checks+=1
p=fresh('special')
with closing(sqlite3.connect(p)) as c, c:
 ids={1,2}; ids.update(x[0] for x in c.execute('SELECT user_id FROM posts ORDER BY id DESC LIMIT 20'))
 names=['quote'+chr(34)+' slash'+chr(92)+' controls'+''.join(map(chr,range(32)))+' 中文é 😀', 'x'*120+chr(34)+chr(92)+chr(1)]
 for i,uid in enumerate(ids): c.execute('UPDATE users SET username=? WHERE id=?',(names[i%2]+str(uid),uid))
full=run(p); verify(p,full); checks+=1
verify(p,run(p,'counts')); checks+=1
for extra in ({'NO_AUTHORS':'1'},{'NO_TABLE':'1'},{'NO_AUTHORS':'1','NO_TABLE':'1'}):
 rows=run(p,extra=extra); assert rows==full; verify(p,rows); checks+=1
rows=run(p,'writes'); rows.pop('feed'); verify(p,rows); assert rows['duplicate']==['0']; assert json.loads(rows['after_commit'][0])['post']['like_count']==2; checks+=1
rows=run(p,'move'); verify(p,rows); assert rows['moved_cache']==['1']; checks+=1
p=fresh('sparse')
with closing(sqlite3.connect(p)) as c, c: c.execute('DELETE FROM likes WHERE post_id<100000')
rows=run(p,'lowlike'); rows.pop('feed'); rows.pop('post'); verify(p,rows); assert json.loads(rows['after_lowlike'][0])['post']['like_count']==1; checks+=1
p=fresh('empty')
with closing(sqlite3.connect(p)) as c, c: c.execute('DELETE FROM likes'); c.execute('DELETE FROM posts')
for extra in ({},{'NO_TABLE':'1'},{'NO_AUTHORS':'1','NO_TABLE':'1'}):
 rows=run(p,extra=extra); verify(p,rows); assert json.loads(rows['feed'][0])=={'posts':[]}; checks+=1
print('PASS:',checks,'SQLite/differential cases: escapes, UTF-8, fallbacks, writes, duplicate likes, move, empty feed')
