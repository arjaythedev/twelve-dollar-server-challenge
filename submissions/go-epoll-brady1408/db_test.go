package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
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

func dbPathOf(t *testing.T, db *sqlite.Conn) string {
	t.Helper()
	st, err := db.Prepare("PRAGMA database_list")
	if err != nil {
		t.Fatal(err)
	}
	defer st.Finalize()
	st.Step()
	return st.ColumnText(2)
}

// boundWAL truncates the WAL once it passes the limit, and does nothing below it.
func TestBoundWALTruncates(t *testing.T) {
	db := openTestDB(t)
	s, err := newServer(db, testSecret)
	if err != nil {
		t.Fatal(err)
	}
	defer s.close()
	s.walPath = dbPathOf(t, db) + "-wal"
	if s.boundWAL(1 << 20) {
		t.Fatal("boundWAL ran on an empty WAL")
	}
	for i := 0; i < 1500; i++ { // one small committed transaction each: at least one 4 KiB frame
		if err := s.beginTx(); err != nil {
			t.Fatal(err)
		}
		if err := db.Exec(fmt.Sprintf("INSERT INTO posts (user_id, body) VALUES (1, 'wal %d')", i)); err != nil {
			t.Fatal(err)
		}
		if err := s.commitBatch(); err != nil {
			t.Fatal(err)
		}
	}
	before, err := os.Stat(s.walPath)
	if err != nil || before.Size() < 1<<20 {
		t.Fatalf("WAL should exceed 1 MiB before the test: %v %v", before, err)
	}
	if !s.boundWAL(1 << 20) {
		t.Fatal("boundWAL should have run")
	}
	after, _ := os.Stat(s.walPath)
	if after.Size() != 0 {
		t.Fatalf("WAL is %d bytes after truncation, want 0", after.Size())
	}
	// The connection still works and the rows are there.
	if _, body, _ := do(t, s, "GET /posts/1503 HTTP/1.1\r\n\r\n"); !strings.Contains(body, `"body":"wal 1499"`) {
		t.Fatal(body)
	}
}
