"""Exercise forced checkpoint eligibility while a reader delays background work.

Fixture environment: HPHP_CKPT_FRAMES=32, HPHP_CKPT_FORCE_FRAMES=128,
HPHP_STATS=1. Check its journal for mode=forced after this probe.
"""
import http.client
import argparse
import json
import pathlib
import sqlite3
import time
import uuid

args=argparse.ArgumentParser(description=__doc__)
args.add_argument('--database',required=True)
args.add_argument('--tokens',required=True)
args.add_argument('--port',type=int,default=3000)
opts=args.parse_args()
token=json.loads(pathlib.Path(opts.tokens).read_text())[0]['token']
marker='checkpoint-probe-'+uuid.uuid4().hex
reader=sqlite3.connect(opts.database,timeout=5)
reader.execute('BEGIN')
reader.execute('SELECT count(*) FROM posts').fetchone()
http=http.client.HTTPConnection('127.0.0.1',opts.port,timeout=10)
def post(i):
    http.request('POST','/posts',json.dumps({'body':marker+str(i)}),
                 {'Authorization':'Bearer '+token,'Content-Type':'application/json'})
    response=http.getresponse()
    payload=json.loads(response.read())
    assert response.status==201, (response.status,payload)
try:
    for i in range(120):
        post(i)
        time.sleep(.005)
finally:
    reader.rollback()
for i in range(120,136):
    post(i)
    time.sleep(.005)
http.close()
persisted=reader.execute('SELECT count(*) FROM posts WHERE body LIKE ?', (marker+'%',)).fetchone()[0]
reader.close()
assert persisted==136, persisted
print(json.dumps({'acknowledged_posts':136,'persisted_posts':persisted,'reader_released':True}))
