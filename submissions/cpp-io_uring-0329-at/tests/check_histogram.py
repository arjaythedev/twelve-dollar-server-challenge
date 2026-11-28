import array, shutil, sqlite3, struct, subprocess, sys
from pathlib import Path
B=Path(sys.argv[1]); p=B/'histogram.db'
shutil.copyfile(str(Path(sys.argv[2])/'seed/feed.db'),p)
subprocess.run([str(B/'histogram_dump'),str(p),str(B/'counts.bin')],check=True)
raw=(B/'counts.bin').read_bytes(); lo,hi=struct.unpack('qq',raw[:16]); counts=array.array('I'); counts.frombytes(raw[16:])
c=sqlite3.connect(p); expected=dict(c.execute('SELECT post_id,count(*) FROM likes GROUP BY post_id')); total=c.execute('SELECT count(*) FROM likes').fetchone()[0]; c.close()
bad=[(lo+i,n,expected.get(lo+i,0)) for i,n in enumerate(counts) if n!=expected.get(lo+i,0)]
assert not bad,(len(bad),bad[:10]); assert sum(counts)==total; assert len(counts)==hi-lo+1
print('HISTOGRAM_PASS',dict(buckets=len(counts),groups=len(expected),likes=total,mismatches=len(bad)))
