package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

// Small pages force like scans across leaf boundaries and interior cells.
// Compare complete responses against SQLite's reference query.
func TestLikeIndexPages(t *testing.T) {
	sqlite, err := exec.LookPath("sqlite3")
	if err != nil {
		t.Skip("sqlite3 CLI required")
	}
	schema, err := os.ReadFile("../../schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "feed.db")
	sql := "PRAGMA page_size=512;" + string(schema) + `
 WITH RECURSIVE ids(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM ids WHERE id<4096)
 INSERT INTO users(id,username) SELECT id,'user-'||id FROM ids;
 INSERT INTO posts(id,user_id,body,created_at) VALUES
 (1,1,'first','2026-01-01'),(2001,2,'middle','2026-01-01'),(5001,3,'last','2026-01-01');
 WITH RECURSIVE ids(id) AS (VALUES(10) UNION ALL SELECT id+1 FROM ids WHERE id<34)
 INSERT INTO posts(id,user_id,body,created_at) SELECT id,4,'older','2020-01-01' FROM ids;
 INSERT INTO posts(id,user_id,body,created_at) VALUES(1000,4,'outside feed','2000-01-01');
 INSERT INTO likes(user_id,post_id) SELECT users.id,posts.id FROM users CROSS JOIN posts
 WHERE posts.id IN(1,1000,2001,5001);
 `
	cmd := exec.Command(sqlite, path)
	cmd.Stdin = bytes.NewBufferString(sql)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("fixture: %v %s", err, out)
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	var reference []byte
	for _, mode := range []string{"1", "0"} {
		t.Setenv("HKN_SQL_READS", mode)
		if err := dbOpen(path); err != nil {
			t.Fatal(err)
		}
		got := newApp(nil).feed(make([]byte, 0, 65536))
		ok := dbCommit()
		dbClose()
		if !ok || !bytes.HasPrefix(got, []byte("HTTP/1.1 200 ")) || bytes.Count(got, []byte(`"like_count":4096`)) != 3 {
			t.Fatalf("mode %s: %s", mode, got)
		}
		if mode == "1" {
			reference = got
		} else if !bytes.Equal(got, reference) {
			t.Fatalf("fast and SQL responses differ: %s vs %s", got, reference)
		}
	}
}
