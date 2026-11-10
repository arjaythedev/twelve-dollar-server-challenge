package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

var integerIDs = []int64{
	-9223372036854775808, -140737488355329, -2147483649, -8388609,
	-32769, -129, -1, 0, 1, 127, 128, 32768, 8388608,
	2147483648, 140737488355328, 9223372036854775807,
}

// Run independent schema fixtures without disturbing other database tests.
func TestFeedPlanCases(t *testing.T) {
	sqlite, err := exec.LookPath("sqlite3")
	if err != nil {
		t.Skip("sqlite3 CLI required")
	}
	schema, err := os.ReadFile("../../schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	const sparse = `
INSERT INTO users(id,username) VALUES(1,'alice'),(2,'bob');
INSERT INTO posts(id,user_id,body,created_at) VALUES
 (1,1,'body','2025-12-31T23:59:19.409Z'),
 (2001,1,'body','2025-12-31T23:59:19.409Z'),
 (5001,1,'body','2025-12-31T23:59:19.409Z');
INSERT INTO likes(user_id,post_id) VALUES(1,1),(2,1),(1,2001);
`
	for _, name := range []string{"empty", "sparse", "orphan", "mixed", "blob", "schema", "utf16", "overflow_page", "integers"} {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "feed.db")
			sql := string(schema)
			if name == "utf16" {
				sql = "PRAGMA encoding='UTF-16le';" + sql
			}
			if name != "empty" && name != "integers" {
				sql += sparse
			}
			if name == "orphan" {
				sql += `INSERT INTO posts(id,user_id,body,created_at) VALUES(9001,999,'orphan','2026-01-01T00:00:00.000Z');`
			}
			switch name {
			case "integers":
				for _, id := range integerIDs {
					sql += fmt.Sprintf("INSERT INTO users(id,username) VALUES(%d,'u%d'); INSERT INTO posts(id,user_id,body,created_at) VALUES(%d,%d,'body','2025-12-31T23:59:19.409Z'); INSERT INTO likes(user_id,post_id) VALUES(%d,%d);", id, id, id, id, id, id)
				}
			case "mixed":
				sql += `UPDATE posts SET created_at='2025-12-30T23:59:19.409Z',user_id=2 WHERE id=2001; UPDATE posts SET created_at='2026-01-01T00:00:00.000Z' WHERE id=1;`
			case "blob":
				sql += `UPDATE posts SET created_at=CAST(created_at AS BLOB);`
			case "schema":
				sql += `DROP INDEX posts_created_at_id_idx; CREATE INDEX posts_created_at_id_idx ON posts(created_at ASC,id ASC);`
			case "overflow_page":
				sql += `UPDATE users SET username=printf('%.*c',5000,'a') WHERE id=1;`
			}
			cmd := exec.Command(sqlite, path)
			cmd.Stdin = bytes.NewBufferString(sql)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("fixture: %v %s", err, out)
			}
			for _, mode := range []string{"0", "1"} {
				cmd = exec.Command(os.Args[0], "-test.run=^TestFeedPlanChild$")
				cmd.Env = append(os.Environ(), "FEED_PLAN_DB="+path, "FEED_PLAN_CASE="+name, "HKN_SQL_READS="+mode)
				if out, err := cmd.CombinedOutput(); err != nil {
					t.Fatalf("feed SQL=%s: %v %s", mode, err, out)
				}
			}
		})
	}
}

func TestFeedPlanChild(t *testing.T) {
	path := os.Getenv("FEED_PLAN_DB")
	if path == "" {
		t.Skip("run through TestFeedPlanCases")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := dbOpen(path); err != nil {
		t.Fatal(err)
	}
	response := newApp(nil).feed(make([]byte, 0, 65536))
	if !dbCommit() || !bytes.HasPrefix(response, []byte("HTTP/1.1 200 ")) {
		t.Fatalf("feed: %s", response)
	}
	_, body, _ := bytes.Cut(response, []byte("\r\n\r\n"))
	var decoded struct {
		Posts []struct {
			ID           int64
			Body, Author string
			Likes        int `json:"like_count"`
		}
	}
	if err := json.Unmarshal(body, &decoded); err != nil {
		t.Fatal(err)
	}
	if os.Getenv("FEED_PLAN_CASE") == "empty" {
		if string(body) != `{"posts":[]}` {
			t.Fatalf("empty feed: %s", body)
		}
		return
	}
	if os.Getenv("FEED_PLAN_CASE") == "integers" {
		if len(decoded.Posts) != len(integerIDs) {
			t.Fatalf("integer rows: %s", body)
		}
		for i, id := range integerIDs {
			post := decoded.Posts[len(integerIDs)-1-i]
			if post.ID != id || post.Author != fmt.Sprintf("u%d", id) || post.Likes != 1 {
				t.Fatalf("integer feed %d: %+v", id, post)
			}
			detail, status := dbPost(id)
			if status != 1 || detail.id != id || detail.likes != 1 || string(detail.author) != post.Author || !dbCommit() {
				t.Fatalf("integer detail %d: status=%d record=%+v", id, status, detail)
			}
		}
		return
	}
	if len(decoded.Posts) != 3 {
		t.Fatalf("rows: %s", body)
	}
	ids := []int64{5001, 2001, 1}
	if os.Getenv("FEED_PLAN_CASE") == "mixed" {
		ids = []int64{1, 5001, 2001}
	}
	likes := map[int64]int{1: 2, 2001: 1, 5001: 0}
	for i, id := range ids {
		post := decoded.Posts[i]
		author := "alice"
		if os.Getenv("FEED_PLAN_CASE") == "mixed" && id == 2001 {
			author = "bob"
		}
		if os.Getenv("FEED_PLAN_CASE") == "overflow_page" {
			author = string(bytes.Repeat([]byte{'a'}, 5000))
		}
		if post.ID != id || post.Likes != likes[id] || post.Author != author || post.Body != "body" {
			t.Fatalf("row %d: %+v", i, post)
		}
	}
}
