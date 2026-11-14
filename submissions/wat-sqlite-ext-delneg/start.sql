-- Read by the stock sqlite3 shell (start.sh), on the database at $SQLITE_PATH.
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;      -- with WAL: a commit is durable once written to the WAL (SPEC rule 6 allows it)
PRAGMA locking_mode=EXCLUSIVE;  -- one connection: no file locks per transaction, the WAL index on the heap
PRAGMA cache_size=-65536;       -- 64 MiB page cache
.load ./build/serve
SELECT serve('build/app.so');   -- runs the server; never returns
