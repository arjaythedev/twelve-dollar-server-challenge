package main

import (
	"os"
	"sync"
	"time"

	"feedapi/internal/sqlite"
)

const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id`

const (
	sqlFeed   = postSelect + ` ORDER BY p.created_at DESC, p.id DESC LIMIT 20`
	sqlPost   = postSelect + ` WHERE p.id = ?`
	sqlCreate = `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`
	sqlLike   = `INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING`
)

// Shared by both connections. journal_mode=WAL is already persisted in the seed file;
// setting it again is harmless and makes the requirement explicit.
const commonPragmas = `
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
PRAGMA temp_store=MEMORY;`

// Loop connection: the 250 MB file is read through mmap, an 8 MiB page cache
// covers the hot index pages, and checkpoints are left to the checkpointer thread.
const loopPragmas = commonPragmas + `
PRAGMA busy_timeout=5000;
PRAGMA mmap_size=268435456;
PRAGMA cache_size=-8192;
PRAGMA wal_autocheckpoint=0;`

// Checkpointer: a short busy timeout so it never lingers on a lock; a missed
// pass is simply retried on the next tick.
const checkpointPragmas = commonPragmas + `
PRAGMA busy_timeout=100;`

func openLoopConn(path string) (*sqlite.Conn, error) {
	return openWith(path, loopPragmas)
}

func openCheckpointConn(path string) (*sqlite.Conn, error) {
	return openWith(path, checkpointPragmas)
}

func openWith(path, pragmas string) (*sqlite.Conn, error) {
	db, err := sqlite.Open(path)
	if err != nil {
		return nil, err
	}
	if err := db.Exec(pragmas); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

type statements struct {
	feed, post, create, like, health, begin, commit, rollback, truncate *sqlite.Stmt
}

type server struct {
	db      *sqlite.Conn
	stmts   statements
	auth    *verifier
	start   time.Time
	inTx    bool
	scratch []byte
	walPath string // SQLITE_PATH + "-wal"; empty disables boundWAL
}

// checkpointMu serialises the checkpointer's PASSIVE checkpoints with the
// loop's TRUNCATE so the loop never waits in SQLite's busy handler for one.
var checkpointMu sync.Mutex

func newServer(db *sqlite.Conn, secret []byte) (*server, error) {
	s := &server{db: db, auth: newVerifier(secret), start: time.Now(), scratch: make([]byte, 0, 16384)}
	var err error
	prep := func(dst **sqlite.Stmt, sql string) {
		if err != nil {
			return
		}
		*dst, err = db.Prepare(sql)
	}
	prep(&s.stmts.feed, sqlFeed)
	prep(&s.stmts.post, sqlPost)
	prep(&s.stmts.create, sqlCreate)
	prep(&s.stmts.like, sqlLike)
	prep(&s.stmts.health, `SELECT 1`)
	prep(&s.stmts.begin, `BEGIN IMMEDIATE`)
	prep(&s.stmts.commit, `COMMIT`)
	prep(&s.stmts.rollback, `ROLLBACK`)
	prep(&s.stmts.truncate, `PRAGMA wal_checkpoint(TRUNCATE)`)
	if err != nil {
		s.close()
		return nil, err
	}
	return s, nil
}

// beginTx opens the batch transaction if none is open. Writes from one epoll
// batch share it; commitBatch ends it before their responses are flushed.
func (s *server) beginTx() error {
	if s.inTx {
		return nil
	}
	if _, err := s.stmts.begin.Step(); err != nil {
		return err
	}
	s.stmts.begin.Reset()
	s.inTx = true
	return nil
}

// commitBatch commits the open transaction. On failure it rolls back and
// returns the commit error; the caller turns every held response into a 500.
func (s *server) commitBatch() error {
	if !s.inTx {
		return nil
	}
	s.inTx = false
	_, err := s.stmts.commit.Step()
	s.stmts.commit.Reset()
	if err != nil {
		s.stmts.rollback.Step()
		s.stmts.rollback.Reset()
		return err
	}
	return nil
}

// boundWAL truncates the WAL from the loop thread once the file passes limit
// bytes. SQLite only restarts a WAL when a writer begins while the checkpoint
// has fully caught up, which never happens while every batch commits; the
// checkpointer's PASSIVE passes keep the backlog tiny, so this TRUNCATE copies
// a few frames, syncs, and zeroes the file. It is skipped (and retried on the
// next sweep) if the checkpointer is mid-pass.
func (s *server) boundWAL(limit int64) bool {
	if s.walPath == "" || s.inTx {
		return false
	}
	fi, err := os.Stat(s.walPath)
	if err != nil || fi.Size() < limit {
		return false
	}
	if !checkpointMu.TryLock() {
		return false
	}
	defer checkpointMu.Unlock()
	s.stmts.truncate.Step()
	s.stmts.truncate.Reset()
	return true
}

func (s *server) close() {
	for _, st := range []*sqlite.Stmt{s.stmts.feed, s.stmts.post, s.stmts.create, s.stmts.like, s.stmts.health, s.stmts.begin, s.stmts.commit, s.stmts.rollback, s.stmts.truncate} {
		if st != nil {
			st.Finalize()
		}
	}
	s.db.Close()
}
