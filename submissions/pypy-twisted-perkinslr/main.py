"""The $12 server challenge API"""

import sys
pypy = 'pypy' in sys.executable
import base64
import hashlib
import hmac
import os
import re
import sqlite3
import time
import json
import twisted
from twisted.web.server import Site
from twisted.web.resource import Resource
from twisted.web.http import Request
from twisted.internet.tcp import Port
from twisted.internet.epollreactor import EPollReactor
from twisted.internet.defer import succeed, fail, Deferred

reactor: EPollReactor
from twisted.internet import reactor

# Performance tuning.  Higher values give more throughput, but higher latency
if pypy:
  QUEUE_DEPTH = 150
  BATCH_DELAY = 0.30
else:
  QUEUE_DEPTH = 75
  BATCH_DELAY = 0.35

# Configuration
SQLITE_PATH = os.environ["SQLITE_PATH"]
JWT_SECRET = os.environ["JWT_SECRET"].encode()
HOST = os.environ.get("HOST", "0.0.0.0")
PORT = int(os.environ.get("PORT", 3000))


# SQL Statements
POST_SELECT = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id
"""

FEED_SQL = POST_SELECT + " ORDER BY p.id DESC, p.id DESC LIMIT 20"


INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES {values} RETURNING id, user_id, body, created_at"


INSERT_LIKE_SQL = """
INSERT INTO likes (user_id, post_id)
values {values}
on conflict do nothing
returning user_id, post_id
"""

SQL_LIKERS = [None,
  *[INSERT_LIKE_SQL.replace('{values}', str.join(', ', ('(?, ?)',)*i)) for i in range(1, QUEUE_DEPTH * 2 + 1)]]

SQL_INSERTERS = [None,
  *[INSERT_POST_SQL.replace('{values}', str.join(', ', ('(?, ?)',)*i)) for i in range(1, QUEUE_DEPTH * 2 + 1)]]

SQL_READERS = [None,
  POST_SELECT + " WHERE p.id = ?",
  *[(POST_SELECT + f" WHERE p.id in ({str.join(', ', list('?' * i))})") for i in range(2, QUEUE_DEPTH * 2 + 1)]
]


# yas-style-snippets

YAS_STATUS_OK = lambda ut: b"""{"status":"ok","db":"ok","uptime_s":""" + str(ut).encode() + b"}"
YAS_STATUS_ERR = lambda err: b"""{"status":"degraded","db":"unreachable","error":""" + err + b"}"

YAS_ERROR = lambda err: b'''{"error":"'''+err+b'''"}'''

YAS_POST = lambda _id, body, created_at, author, like_count: '{"id":'+str(_id)+',"body":"' + body.replace('"',r'\"') + '","created_at":"' + created_at + '","author":"' + author.replace('"', r'\"') + '","like_count":' + str(like_count) + '}'

YAS_LIKE = lambda already, pid: '{"liked":true,"already_liked":' + already + ',"post_id":' + str(pid) + '}'

def makeFeed(cur):
  cur.execute(FEED_SQL)
  return ('{"posts":[' + str.join(",", (YAS_POST(*row) for row in cur.fetchall())) + ']}').encode('utf-8')

class SQLite:
  "Queues and deduplicates database queries, remembering the request that generated them so the response gets back to the correct client"
  def __init__(self, path):
    db = sqlite3.connect(path, check_same_thread=False, cached_statements=QUEUE_DEPTH * 3+30)
    db.execute("PRAGMA journal_mode = WAL")
    db.execute("PRAGMA synchronous = NORMAL")
    db.execute("PRAGMA mmap_size = 1073741824")
    db.execute("PRAGMA cache_size = -1048576")
    db.execute("PRAGMA temp_store = MEMORY")
    self.db = db
    self.cursor = db.cursor()
    self.readqueue = {}
    self.feedqueue = []
    self.likequeue = []
    self.newqueue = []
    self.healthqueue = []
    self.totalReading = 0
    self.feedTimer = twisted.internet.reactor.callLater(BATCH_DELAY, self.checkFeed)
    self.writeTimer = twisted.internet.reactor.callLater(BATCH_DELAY, self.checkWrites)
    self.readTimer = twisted.internet.reactor.callLater(BATCH_DELAY, self.checkReads)
    twisted.internet.reactor.callLater(BATCH_DELAY, self.checkHealth)
    self.cursor.execute("select max(id) from posts");
    self.max_pid = self.cursor.fetchone()[0]
    self.cursor.execute('select count(id) from posts');
    assert(self.max_pid == self.cursor.fetchone()[0]) # posts is a dense matrix, so we don't have to worry about holes.

  def new(self, req, user_id):
    try:
      body = json.loads(req.content.read())
    except:
      req.setResponseCode(400)
      req.write(YAS_ERROR(b'malformed JSON body'))
      req.finish()
      return 1
    pbody = body.get('body', '')
    if not isinstance(pbody, str) or not(pbody:=pbody.strip()):
      req.setResponseCode(400)
      req.write(YAS_ERROR(b'body is required'))
      req.finish()
      return 1
    if len(pbody) > 500:
      req.setResponseCode(400)
      req.write(YAS_ERROR(b'body must be at most 500 characters'))
      req.finish()
      return 1

    args = (user_id, pbody)

    self.newqueue.append((args, req))
    if len(self.newqueue) + len(self.likequeue) >= QUEUE_DEPTH:
      self.checkWrites()
    return 1

  def like(self, req, user_id):
    spid = req.postpath[1]
    
    if not spid.isdigit():
      req.setResponseCode(400)
      req.write(YAS_ERROR(b'invalid post id'))
      req.finish()
      return 1

    pid = int(spid)
    if pid < 0 or pid > self.max_pid:
      req.setResponseCode(404)
      req.write(YAS_ERROR(b'post not found'))
      req.finish()
      return 1
    self.likequeue.append(((user_id, pid), req))
    if len(self.newqueue) + len(self.likequeue) >= QUEUE_DEPTH:
      self.checkWrites()
    return 1

  def checkWrites(self):
    if not self.writeTimer.called:
      self.writeTimer.reset(BATCH_DELAY / 2)
    else:
      self.writeTimer = twisted.internet.reactor.callLater(BATCH_DELAY, self.checkWrites)
    if self.newqueue:
      args = [x for pair in self.newqueue for x in pair[0]]
      count = len(args) // 2
      if count > QUEUE_DEPTH:
        count = QUEUE_DEPTH
      self.cursor.execute(SQL_INSERTERS[count], args[:100])
      for pid, uid, body, created_time in self.cursor.fetchall():
        if pid > self.max_pid:
          self.max_pid = pid
        for idx, arg_req in enumerate(self.newqueue):
          if arg_req[0] == (uid, body):
            self.newqueue.pop(idx)
            arg_req[1].setResponseCode(201)
            arg_req[1].write(('{"post":' + YAS_POST(str(pid), body, created_time, arg_req[1].username, 0) + '}').encode('utf-8'))
            if not arg_req[1]._disconnected:
              arg_req[1].finish()
            break

    if self.likequeue:
      args = [x for pair in self.likequeue for x in pair[0]]
      self.cursor.execute(SQL_LIKERS[len(args)//2], args)
      for user_post in reversed(self.cursor.fetchall()):
        for idx, ent in enumerate(self.likequeue):
          if ent[0] == user_post:
            self.likequeue.pop(idx)
            ent[1].setResponseCode(201)
            ent[1].write(YAS_LIKE('false', ent[0][1]).encode('utf-8'))
            if not ent[1]._disconnected:
              ent[1].finish()
            break
      for user_post, req in self.likequeue:
        req.write(YAS_LIKE('true', user_post[1]).encode('utf-8'))
        if not req._disconnected:
          req.finish()
      self.likequeue.clear()
  def checkReads(self):
    if not self.readTimer.called:
      self.readTimer.reset(BATCH_DELAY / 2)
    else:
      self.readTimer = twisted.internet.reactor.callLater(BATCH_DELAY, self.checkReads)
    if not self.readqueue:
      return
    pids = list(self.readqueue)
    pids.sort()
    self.cursor.execute(SQL_READERS[len(pids)], pids)
    for post in self.cursor.fetchall():
      response = b'{"post":'+YAS_POST(*post).encode('utf-8')+b'}'
      for r in self.readqueue.pop(post[0]):
        r.write(response)
        if not r._disconnected:
          r.finish()
    for pid, reqs in self.readqueue.items():
       for r in reqs:
         r.setResponseCode(404)
         r.write(YAS_ERROR(b'post not found'))
         if not r._disconnected:
           r.finish()
    self.readqueue.clear()
    self.totalReading = 0
  def post(self, req):
    spid = req.postpath[-1]
    if (not spid.isdigit()) or (pid:=int(spid)) < 1:
      req.setResponseCode(400)
      req.write(YAS_ERROR(b'invalid post id'))
      req.finish()
      return 1
    waiting = self.readqueue.get(pid, None)
    if waiting is None:
      waiting = [req]
      self.readqueue[pid] = waiting
    else:
      waiting.append(req)
    self.totalReading += 1
    if self.totalReading > QUEUE_DEPTH:
      self.checkReads()
    return 1

  def checkHealth(self):
    twisted.internet.reactor.callLater(BATCH_DELAY, self.checkHealth)
    if self.healthqueue:
      self.cursor.execute("select 1")
      res = self.cursor.fetchall()
      if res == [(1,)]:
        result = YAS_STATUS_OK(int(time.monotonic() - START))
        code = 200
      else:
        result = YAS_STATUS_ERR(repr(res).encode('utf-8'))
        code = 503
      for r in self.healthqueue:
        r.setResponseCode(code)
        r.write(result)
        if not r._disconnected:
          r.finish()
      self.healthqueue.clear()

  def health(self, req):
    self.healthqueue.append(req)
    return 1

  def feed(self, req):
    self.feedqueue.append(req)
    if len(self.feedqueue) > QUEUE_DEPTH:
      self.checkFeed()
    return 1
  
  def checkFeed(self):
    if self.feedTimer.called:
      self.feedTimer = twisted.internet.reactor.callLater(BATCH_DELAY, self.checkFeed)
    else:
      self.feedTimer.reset(BATCH_DELAY / 2)

    if not self.feedqueue:
      return
    feed = makeFeed(self.cursor)
    while self.feedqueue:
      r = self.feedqueue.pop()
      r.write(feed)
      if not r._disconnected:
        r.finish()
    self.feedqueue.clear()

class Root(Resource):
  isLeaf=True
  def __init__(self, db):
    self.db = db
    super().__init__()

  def fail(self, req, code, reason):
    req.setResponseCode(code)
    req.write(YAS_ERROR(reason))
    req.finish()
    return 1

  def auth(self, req):
    header = req.getHeader(b"authorization")
    if header is None or not header.startswith(b"Bearer "):
      return self.fail(req, 401, b'missing bearer token')

    sections = header[7:].split(b'.')
    if len(sections) != 3:
      return self.fail(req, 401, b'invalid or expired token')
    head_b64, payload_b64, sig_b64 = sections
    head = json.loads(base64.urlsafe_b64decode(head_b64 + b"=" * ((4 - (len(head_b64) & 3)) & 3)))
    if not isinstance(head, dict) or head.get("alg") != "HS256":
      return self.fail(req, 401, b"invalid or expired token")
    digest = hmac.new(JWT_SECRET, (head_b64 + b"." + payload_b64), hashlib.sha256).digest()
    expected = base64.urlsafe_b64encode(digest).rstrip(b"=")
    if not hmac.compare_digest(expected, sig_b64):
      return self.fail(req, 401, b"invalid or expired token")

    payload = json.loads(base64.urlsafe_b64decode(payload_b64 + b"=" * ((4 - (len(payload_b64) & 3)) & 3)))
    if not isinstance(payload, dict):
      return self.fail(req, 401, b"invalid or expired token")
    now = int(time.time())
    exp = payload.get("exp")
    if exp is not None and (not isinstance(exp, (int, float)) or isinstance(exp, bool) or now >= exp):
      return self.fail(req, 401, b"invalid or expired token")
    nbf = payload.get("nbf")
    if nbf is not None and (not isinstance(nbf, (int, float)) or isinstance(nbf, bool) or now < nbf):
      return self.fail(req, 401, b"invalid or expired token")
    sub = payload.get("sub")
    username = payload.get("username")
    user_id = (sub.isdigit() and int(sub)) or None
    if user_id is None or not username:
      return self.fail(req, 401, b"invalid token payload")
    if len(req.postpath) != 1:
      return self.db.like(req, user_id)
    req.username = username
    return self.db.new(req, user_id)

  def render(self, req):
    req.setHeader("Content-Type", b"application/json")
    if req.method == b'POST':
        return self.auth(req)

    if req.path == b'/health':
        return self.db.health(req)

    if req.path == b'/feed':
        return self.db.feed(req)


    if req.postpath[0] == b'posts':
        return self.db.post(req)

    req.setResponseCode(404)
    req.write(YAS_ERROR(b"not found"))
    req.finish()
    return 1

def setgc():
  import gc
  gc.collect()
  gc.freeze()
  gc.set_threshold(50000, 20, 20)


START = time.monotonic()

if __name__ == '__main__':
  db = SQLite(SQLITE_PATH)
  listener = reactor.listenTCP(PORT, Site(Root(db)), interface=HOST)
  if not pypy:
    reactor.callWhenRunning(setgc)
  reactor.run()
  raise SystemExit()


