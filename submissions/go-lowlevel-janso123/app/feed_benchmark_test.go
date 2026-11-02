//go:build linux && cgo_sqlite

package main

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

// Test-only copy of epoll-loop's read body, called on the same locked thread
// and fixture as the candidate. It excludes the unchanged HTTP parser/socket.
func baselineFeedRead(statement *sql.Stmt) reply {
	rows, err := statement.Query()
	if err != nil {
		return failure(500, "internal server error")
	}
	defer rows.Close()
	posts := make([]Post, 0, 20)
	for rows.Next() {
		var p Post
		if rows.Scan(&p.ID, &p.Body, &p.Created, &p.Author, &p.Likes) != nil {
			return failure(500, "internal server error")
		}
		posts = append(posts, p)
	}
	if rows.Err() != nil {
		return failure(500, "internal server error")
	}
	data, err := json.Marshal(struct {
		Posts []Post `json:"posts"`
	}{posts})
	if err != nil {
		return failure(500, "internal server error")
	}
	return reply{200, data}
}

func BenchmarkFeedPath(b *testing.B) {
	runtime.GOMAXPROCS(1)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	path := filepath.Join(b.TempDir(), "feed.db")
	if err := os.WriteFile(path, nil, 0600); err != nil {
		b.Fatal(err)
	}
	db, c, err := openSQLite(path, &bytes.Buffer{})
	if err != nil {
		b.Fatal(err)
	}
	defer db.Close()
	defer c.Close()
	for _, query := range []string{
		`CREATE TABLE users(id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE)`,
		`CREATE TABLE posts(id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id),body TEXT NOT NULL,created_at TEXT NOT NULL)`,
		`CREATE TABLE likes(user_id INTEGER NOT NULL REFERENCES users(id),post_id INTEGER NOT NULL REFERENCES posts(id),PRIMARY KEY(user_id,post_id))`,
		`INSERT INTO users VALUES(1,'fixture')`,
	} {
		if _, err := c.ExecContext(ctx, query); err != nil {
			b.Fatal(err)
		}
	}
	for id := 1; id <= 20; id++ {
		if _, err := c.ExecContext(ctx, `INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,'a <>& "quote" 😀 U+2028 ','2025-01-01T00:00:00.000Z')`, id); err != nil {
			b.Fatal(err)
		}
	}
	a, err := newApp(c, []byte("benchmark"), path)
	if err != nil {
		b.Fatal(err)
	}
	defer a.close()
	const read = `SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20`
	statement, err := c.PrepareContext(ctx, read)
	if err != nil {
		b.Fatal(err)
	}
	defer statement.Close()
	for _, variant := range []string{"epoll-loop", "epoll-cfeed"} {
		b.Run(variant, func(b *testing.B) {
			b.ReportAllocs()
			before := runtime.NumCgoCall()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				var r reply
				if variant == "epoll-loop" {
					r = baselineFeedRead(statement)
				} else {
					r = a.handle(job{method: "GET", path: "/feed"})
				}
				if r.status != 200 {
					b.Fatal(r.status)
				}
			}
			b.StopTimer()
			b.ReportMetric(float64(runtime.NumCgoCall()-before)/float64(b.N), "cgo/op")
		})
	}
}
