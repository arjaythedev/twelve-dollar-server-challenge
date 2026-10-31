//go:build cgo_sqlite && !pure_sqlite

package main

import (
	"bytes"
	"encoding/json"
	"math/rand"
	"strings"
	"testing"
)

func TestCJSONQuoteMatchesGo(t *testing.T) {
	inputs := []string{"", "plain", "\"\\\n\r\t\x00\x01\x1f", "<script>&</script>", "\u2028\u2029", "😀🦄", "\xff\xfe", "a\xc0\xafz"}
	rng := rand.New(rand.NewSource(7319))
	for i := 0; i < 10000; i++ {
		b := make([]byte, rng.Intn(1000))
		_, _ = rng.Read(b)
		inputs = append(inputs, string(b))
	}
	for i, input := range inputs {
		want, err := json.Marshal(input)
		if err != nil {
			t.Fatal(err)
		}
		got := cQuoteForTest(input)
		if !bytes.Equal(got, want) {
			t.Fatalf("case %d: got %q want %q", i, got, want)
		}
	}
}

func TestCReadsMatchGoAndOwnReplies(t *testing.T) {
	a, _ := fixture(t, testSchema)
	for id := 2; id <= 20; id++ {
		body := strings.Repeat("<&😀\u2028\\\"", 50)
		if _, err := a.conn.ExecContext(ctx, "INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,?,'2025-01-01T00:00:00.000Z')", id, body); err != nil {
			t.Fatal(err)
		}
	}
	for _, path := range []string{"/feed", "/posts/1", "/posts/20", "/posts/999999"} {
		j := job{method: "GET", path: path}
		got := a.handle(j)
		if path == "/posts/999999" && (got.status != 404 || string(got.data) != `{"error":"post not found"}`) {
			t.Fatalf("%s: %d %q", path, got.status, got.data)
		} else if path != "/posts/999999" && got.status != 200 {
			t.Fatalf("%s: %d %q", path, got.status, got.data)
		}
	}
	first := a.handle(job{method: "GET", path: "/feed"})
	wantFirst := bytes.Clone(first.data)
	if _, err := a.conn.ExecContext(ctx, "UPDATE posts SET body='changed' WHERE id=20"); err != nil {
		t.Fatal(err)
	}
	_ = a.handle(job{method: "GET", path: "/feed"})
	if !bytes.Equal(first.data, wantFirst) {
		t.Fatal("reply overwritten by loop buffer")
	}
}

func TestCReadBufferGrowsAndKeepsPriorReply(t *testing.T) {
	a, _ := fixture(t, testSchema)
	for id := 2; id <= 21; id++ {
		if _, err := a.conn.ExecContext(ctx, "INSERT INTO posts(id,user_id,body,created_at) VALUES(?,1,?,'2025-01-01T00:00:00.000Z')", id, strings.Repeat("<&😀\\\"", 100)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := a.conn.ExecContext(ctx, "UPDATE users SET username=? WHERE id=1", strings.Repeat("A", 7000)); err != nil {
		t.Fatal(err)
	}
	initial := len(a.reader.buf)
	first := a.handle(job{method: "GET", path: "/posts/1"})
	before := bytes.Clone(first.data)
	large := a.handle(job{method: "GET", path: "/feed"})
	if large.status != 200 || len(large.data) <= initial || cap(a.reader.buf) < len(large.data) {
		t.Fatalf("oversize read: status=%d len=%d initial=%d cap=%d", large.status, len(large.data), initial, cap(a.reader.buf))
	}
	if !bytes.Equal(first.data, before) {
		t.Fatal("earlier response changed")
	}
}
