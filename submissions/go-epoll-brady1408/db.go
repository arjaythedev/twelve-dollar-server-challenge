package main

import (
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
// covers the hot index pages, checkpoints are left to the checkpointer thread,
// and the WAL file is held at 64 MiB once it has grown that far (see loop.checkpointTick).
const loopPragmas = commonPragmas + `
PRAGMA busy_timeout=5000;
PRAGMA mmap_size=268435456;
PRAGMA cache_size=-8192;
PRAGMA wal_autocheckpoint=0;
PRAGMA journal_size_limit=67108864;`

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
	feed, post, create, like, health, begin, commit, rollback *sqlite.Stmt
}

type server struct {
	db      *sqlite.Conn
	stmts   statements
	auth    *verifier
	start   time.Time
	inTx    bool
	scratch []byte
}

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

func (s *server) close() {
	for _, st := range []*sqlite.Stmt{s.stmts.feed, s.stmts.post, s.stmts.create, s.stmts.like, s.stmts.health, s.stmts.begin, s.stmts.commit, s.stmts.rollback} {
		if st != nil {
			st.Finalize()
		}
	}
	s.db.Close()
}
