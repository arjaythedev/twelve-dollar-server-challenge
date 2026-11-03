package main

import (
	"os"
	"path/filepath"
	"testing"

	"feedapi/internal/sqlite"
)

// openTestDB builds a small database from the challenge schema: users 1-2,
// posts 1-3 by user 1 (created_at ascending with id), user 2 likes post 1.
func openTestDB(t *testing.T) *sqlite.Conn {
	t.Helper()
	schema, err := os.ReadFile("../../schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "feed.db")
	c, err := sqlite.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.Exec(string(schema)); err != nil {
		t.Fatal(err)
	}
	if err := c.Exec(`
		INSERT INTO users (id, username) VALUES (1, 'golden_ember_1'), (2, 'other_user');
		INSERT INTO posts (id, user_id, body, created_at) VALUES
		  (1, 1, 'first post',  '2025-01-01T00:00:01.000Z'),
		  (2, 1, 'second post', '2025-01-01T00:00:02.000Z'),
		  (3, 1, 'third "post"', '2025-01-01T00:00:03.000Z');
		INSERT INTO likes (user_id, post_id) VALUES (2, 1);
		PRAGMA journal_mode=WAL;`); err != nil {
		t.Fatal(err)
	}
	c.Close()
	db, err := openLoopConn(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	return db
}

func pragmaInt(t *testing.T, db *sqlite.Conn, name string) int64 {
	t.Helper()
	st, err := db.Prepare("PRAGMA " + name)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Finalize()
	if row, err := st.Step(); err != nil || !row {
		t.Fatalf("pragma %s: row=%v err=%v", name, row, err)
	}
	return st.ColumnInt64(0)
}

func TestLoopConnPragmas(t *testing.T) {
	db := openTestDB(t)
	if got := pragmaInt(t, db, "foreign_keys"); got != 1 {
		t.Fatalf("foreign_keys=%d", got)
	}
	if got := pragmaInt(t, db, "synchronous"); got != 1 { // NORMAL
		t.Fatalf("synchronous=%d", got)
	}
	if got := pragmaInt(t, db, "wal_autocheckpoint"); got != 0 {
		t.Fatalf("wal_autocheckpoint=%d", got)
	}
	st, _ := db.Prepare("PRAGMA journal_mode")
	defer st.Finalize()
	st.Step()
	if mode := st.ColumnText(0); mode != "wal" {
		t.Fatalf("journal_mode=%s", mode)
	}
}

func TestBeginCommitBatch(t *testing.T) {
	s, err := newServer(openTestDB(t), []byte("secret"))
	if err != nil {
		t.Fatal(err)
	}
	defer s.close()
	if err := s.commitBatch(); err != nil {
		t.Fatalf("commit with no tx should be a no-op: %v", err)
	}
	if err := s.beginTx(); err != nil || !s.inTx {
		t.Fatalf("beginTx: %v inTx=%v", err, s.inTx)
	}
	if err := s.beginTx(); err != nil { // idempotent while open
		t.Fatal(err)
	}
	if err := s.db.Exec(`INSERT INTO likes (user_id, post_id) VALUES (1, 2)`); err != nil {
		t.Fatal(err)
	}
	if err := s.commitBatch(); err != nil || s.inTx {
		t.Fatalf("commitBatch: %v inTx=%v", err, s.inTx)
	}
	if got := pragmaInt(t, s.db, "user_version"); got != 0 { // connection still usable
		t.Fatal(got)
	}
}

// SQLite 3.54 re-plans a statement on the first step after a parameter it
// compares with a column is rebound, which recompiles the post query on every
// GET /posts/:id. The loop connection must opt out of that.
func TestPostStatementNotReprepared(t *testing.T) {
	s, err := newServer(openTestDB(t), []byte("secret"))
	if err != nil {
		t.Fatal(err)
	}
	defer s.close()
	st := s.stmts.post
	for i := 0; i < 30; i++ {
		st.BindInt64(1, int64(1+i%3))
		if row, err := st.Step(); err != nil || !row {
			t.Fatalf("step %d: row=%v err=%v", i, row, err)
		}
		st.Reset()
	}
	if n := st.Reprepares(); n != 0 {
		t.Fatalf("post statement re-prepared %d times in 30 runs, want 0", n)
	}
}
