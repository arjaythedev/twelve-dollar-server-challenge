package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

// Exercise a SQL fallback with a record that cannot fit the shared text buffer.
func TestFeedFallbackOverflow(t *testing.T) {
	sqlite, err := exec.LookPath("sqlite3")
	if err != nil {
		t.Skip("sqlite3 CLI required for the fallback fixture")
	}
	schema, err := os.ReadFile("../../schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "feed.db")
	sql := append(schema, []byte(`
INSERT INTO users(id, username) VALUES (1, printf('%.*c', 70000, 'x'));
INSERT INTO posts(user_id, body, created_at) VALUES (1, 'body', CAST('2025-12-31T23:59:19.409Z' AS BLOB));
`)...)
	cmd := exec.Command(sqlite, path)
	cmd.Stdin = bytes.NewReader(sql)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("fixture: %v: %s", err, output)
	}
	// SQLite is process-global; isolate this fixture from other database tests.
	cmd = exec.Command(os.Args[0], "-test.run=^TestFeedFallbackChild$")
	cmd.Env = append(os.Environ(), "FALLBACK_DB="+path)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("fallback: %v: %s", err, output)
	}
}

func TestFeedFallbackChild(t *testing.T) {
	path := os.Getenv("FALLBACK_DB")
	if path == "" {
		t.Skip("run through TestFeedFallbackOverflow")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := dbOpen(path); err != nil {
		t.Fatal(err)
	}
	got := newApp(nil).feed(nil)
	if !dbCommit() || !bytes.HasPrefix(got, []byte("HTTP/1.1 500 ")) {
		t.Fatalf("overflow must return 500, got %s", got)
	}
}
