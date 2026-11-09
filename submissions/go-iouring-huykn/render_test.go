package main

import (
	"bytes"
	"encoding/json"
	"math/rand"
	"os"
	"runtime"
	"testing"
)

type expectedPost struct {
	ID      int64  `json:"id"`
	Body    string `json:"body"`
	Created string `json:"created_at"`
	Author  string `json:"author"`
	Likes   int64  `json:"like_count"`
}

// Check the production serializer against an independent JSON implementation,
// including actual SQL round trips with embedded NULs and multi-byte text.
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
	defer dbClose()
	a := newApp([]byte("test"))
	rng := rand.New(rand.NewSource(42))
	alphabet := []rune("abcXYZ012\\\"\n\r\t\b\f\x00你好🙂")
	for i := 0; i < 300; i++ {
		body := make([]rune, 1+rng.Intn(499))
		for j := range body {
			body[j] = alphabet[rng.Intn(len(alphabet))]
		}
		input := "x" + string(body)
		id, created, ok := dbInsertPost(1, []byte(input))
		if !ok {
			t.Fatalf("insert at %d", i)
		}
		timestamp := string(created) // Copy before another request replaces the arena.
		for user := int64(1); user <= int64(i%12); user++ {
			if dbLike(user, id) != 2 {
				t.Fatal("like")
			}
		}
		response := a.post(make([]byte, 0, 65536), id)
		_, payload, _ := bytes.Cut(response, []byte("\r\n\r\n"))
		var decoded struct {
			Post expectedPost `json:"post"`
		}
		if err := json.Unmarshal(payload, &decoded); err != nil {
			t.Fatal(err)
		}
		want := expectedPost{id, input, timestamp, "golden_ember_1", int64(i % 12)}
		if decoded.Post != want {
			t.Fatalf("SQL/JSON round trip at %d: %+v != %+v", i, decoded.Post, want)
		}
		exact, err := json.Marshal(struct {
			Post expectedPost `json:"post"`
		}{want})
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(payload, exact) {
			t.Fatalf("wire format at %d: %s != %s", i, payload, exact)
		}
		feed := a.feed(make([]byte, 0, 65536))
		_, payload, _ = bytes.Cut(feed, []byte("\r\n\r\n"))
		var recent struct {
			Posts []expectedPost `json:"posts"`
		}
		if err := json.Unmarshal(payload, &recent); err != nil {
			t.Fatal(err)
		}
		found := false
		for _, p := range recent.Posts {
			if p.ID == id {
				found = true
				if p != want {
					t.Fatalf("feed object: %+v", p)
				}
			}
		}
		if !found || len(recent.Posts) != 20 {
			t.Fatal("new post missing from feed")
		}
		guard := bytes.Repeat([]byte{0xa5}, 4096)
		limit := 512 + rng.Intn(2048)
		if got := a.feed(guard[:0:limit]); !bytes.Equal(got, feed) {
			t.Fatalf("buffer growth at %d", i)
		}
		if !bytes.Equal(guard[limit:], bytes.Repeat([]byte{0xa5}, len(guard)-limit)) {
			t.Fatal("write beyond capacity")
		}
		s := &server{app: a, rings: []*ring{{conns: make([]conn, 1)}}, answers: answersPerPass}
		c := &s.rings[0].conns[0]
		*c = conn{gen: 1, flags: fOpen}
		s.serve(0, 0, c, []byte("GET /feed HTTP/1.1\r\nHost: test\r\n\r\n"))
		if len(s.touched) != 1 || c.flags&fSending != 0 || c.wire != 0 || !bytes.Equal(s.buf(c.out), feed) {
			t.Fatal("response sent before commit")
		}
		if !dbCommit() {
			t.Fatal("commit")
		}
	}
}
