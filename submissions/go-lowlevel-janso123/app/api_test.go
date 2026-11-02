package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const testSchema = `CREATE TABLE users(id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE);
CREATE TABLE posts(id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id),body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500),created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));
CREATE TABLE likes(user_id INTEGER NOT NULL REFERENCES users(id),post_id INTEGER NOT NULL REFERENCES posts(id),created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),PRIMARY KEY(user_id,post_id));
INSERT INTO users VALUES(1,'fixture');INSERT INTO posts VALUES(1,1,'seed','2025-01-01T00:00:00.000Z');`

func testToken(t *testing.T, sub string) string {
	t.Helper()
	b64 := base64.RawURLEncoding.EncodeToString
	payload, _ := json.Marshal(map[string]any{"sub": sub, "username": "token author", "exp": time.Now().Unix() + 3600})
	s := b64([]byte(`{"alg":"HS256"}`)) + "." + b64(payload)
	m := hmac.New(sha256.New, []byte("test secret"))
	m.Write([]byte(s))
	return "Bearer " + s + "." + b64(m.Sum(nil))
}
func fixture(t *testing.T, schema string) (*app, string) {
	t.Helper()
	p := filepath.Join(t.TempDir(), "feed.db")
	if e := os.WriteFile(p, nil, 0600); e != nil {
		t.Fatal(e)
	}
	db, c, e := openSQLite(p, &bytes.Buffer{})
	if e != nil {
		t.Fatal(e)
	}
	for _, s := range strings.Split(schema, ";") {
		if strings.TrimSpace(s) != "" {
			if _, e := c.ExecContext(context.Background(), s); e != nil {
				t.Fatal(e)
			}
		}
	}
	a, e := newApp(c, []byte("test secret"), p)
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { a.close(); c.Close(); db.Close() })
	return a, p
}

// Omitting SQLITE_DONE would acknowledge RETURNING before its real autocommit.
func Test201VisibleFromSecondConnection(t *testing.T) {
	a, path := fixture(t, testSchema)
	db, reader, e := openSQLite(path, &bytes.Buffer{})
	if e != nil {
		t.Fatal(e)
	}
	defer db.Close()
	defer reader.Close()
	r := a.handle(job{"POST", "/posts", testToken(t, "1"), []byte(`{"body":" durable "}`)})
	if r.status != 201 {
		t.Fatal(r.status, string(r.data))
	}
	var body string
	if e = reader.QueryRowContext(ctx, "SELECT body FROM posts WHERE id=2").Scan(&body); e != nil || body != "durable" {
		t.Fatal(body, e)
	}
	r = a.handle(job{"POST", "/posts/2/like", testToken(t, "1"), nil})
	if r.status != 201 {
		t.Fatal(r.status, string(r.data))
	}
	var count int
	if e = reader.QueryRowContext(ctx, "SELECT count(*) FROM likes WHERE post_id=2").Scan(&count); e != nil || count != 1 {
		t.Fatal(count, e)
	}
}
func TestFailedAutocommitThenRecovery(t *testing.T) {
	schema := strings.Replace(testSchema, "user_id INTEGER NOT NULL REFERENCES users(id),body", "user_id INTEGER NOT NULL REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED,body", 1)
	a, p := fixture(t, schema)
	db, c, e := openSQLite(p, &bytes.Buffer{})
	if e != nil {
		t.Fatal(e)
	}
	defer db.Close()
	defer c.Close()
	r := a.handle(job{"POST", "/posts", testToken(t, "999"), []byte(`{"body":"orphan"}`)})
	if r.status != 500 || string(r.data) != `{"error":"internal server error"}` {
		t.Fatal(r.status, string(r.data))
	}
	var count int
	if e := c.QueryRowContext(ctx, "SELECT count(*) FROM posts").Scan(&count); e != nil || count != 1 {
		t.Fatal(count, e)
	}
	r = a.handle(job{"POST", "/posts", testToken(t, "1"), []byte(`{"body":"recovered"}`)})
	if r.status != 201 {
		t.Fatal(r.status, string(r.data))
	}
	if e := c.QueryRowContext(ctx, "SELECT count(*) FROM posts").Scan(&count); e != nil || count != 2 {
		t.Fatal(count, e)
	}
}
