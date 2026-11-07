package main

import (
	"bytes"
	"math/rand"
	"os"
	"runtime"
	"testing"
)

// BENCH_DB is a disposable copy: compare the C fast path with the Go fallback
// after real writes, including escapes, Unicode and changing like counts.
func TestRenderDatabase(t *testing.T) {
	path := os.Getenv("BENCH_DB")
	if path == "" {
		t.Skip("set BENCH_DB to a disposable seed database")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := dbOpen(path); err != nil {
		t.Fatal(err)
	}
	a := newApp([]byte("test"))
	rng := rand.New(rand.NewSource(42))
	alphabet := []rune("abcXYZ012\\\"\n\r\t\b\f\x00你好🙂")
	for i := 0; i < 300; i++ {
		body := make([]rune, 1+rng.Intn(499))
		for j := range body {
			body[j] = alphabet[rng.Intn(len(alphabet))]
		}
		id, _, ok := dbInsertPost(1, []byte("x"+string(body)))
		if !ok {
			t.Fatalf("insert at %d", i)
		}
		for user := int64(1); user <= int64(i%12); user++ {
			if dbLike(user, id) != 2 {
				t.Fatal("like")
			}
		}
		fast := a.post(make([]byte, 0, 65536), id)
		slow := a.post(nil, id)
		if !bytes.Equal(fast, slow) {
			t.Fatalf("post renderer differs at %d", i)
		}
		fast = a.feed(fast[:0])
		slow = a.feed(nil)
		if !bytes.Equal(fast, slow) {
			t.Fatalf("feed renderer differs at %d", i)
		}
		// A truncated capacity must fall back without touching the guard bytes.
		guard := bytes.Repeat([]byte{0xa5}, 4096)
		limit := 512 + rng.Intn(2048)
		if got := a.feed(guard[:0:limit]); !bytes.Equal(got, fast) {
			t.Fatalf("small-buffer fallback differs at %d", i)
		}
		if !bytes.Equal(guard[limit:], bytes.Repeat([]byte{0xa5}, len(guard)-limit)) {
			t.Fatal("renderer wrote beyond buffer capacity")
		}
		// No ring is provided: respond must stage bytes without submitting a send.
		c := &connection{in: []byte("GET /feed HTTP/1.1\r\nHost: test\r\n\r\n"), out: make([]byte, 0, 65536)}
		c.used = len(c.in)
		s := &server{app: a}
		if err := s.respond(7, c); err != nil {
			t.Fatal(err)
		}
		if len(s.ready) != 1 || s.ready[0] != 7 || c.sending || !bytes.Equal(c.out, fast) {
			t.Fatal("response was not staged intact")
		}
		if !dbCommit() {
			t.Fatal("commit")
		}
	}
}
