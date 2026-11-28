//go:build linux

package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"os"
	"strconv"
	"testing"
	"time"
)

// Benchmarks of the steps one request goes through, each on its own and then together.
//
//	go test -run xxx -bench . -benchmem
//
// The ones that read the database need a copy of the seed: BENCH_DB=/path/to/copy.db
// (they are skipped without it). BENCH_MAXID is the highest post id asked for.

var benchSecret = []byte("twelve-dollar-challenge")

func benchServer() *server {
	return &server{secret: benchSecret, verify: newVerifier(benchSecret), json: make([]byte, 0, sendBufSize)}
}

func benchToken() string { return benchTokenFor(4211) }

func benchTokenFor(uid int) string {
	enc := base64.RawURLEncoding.EncodeToString
	h := enc([]byte(`{"alg":"HS256","typ":"JWT"}`))
	p := enc([]byte(fmt.Sprintf(`{"sub":"%d","username":"user_%d","exp":%d}`, uid, uid, time.Now().Unix()+86400)))
	m := hmac.New(sha256.New, benchSecret)
	m.Write([]byte(h + "." + p))
	return h + "." + p + "." + enc(m.Sum(nil))
}

// Request heads as k6 sends them.
func benchHead(method, path string, auth bool) []byte {
	h := method + " " + path + " HTTP/1.1\r\nHost: 10.0.0.1\r\nUser-Agent: Grafana k6/1.2.3\r\n"
	if auth {
		h += "Authorization: Bearer " + benchToken() + "\r\n"
	}
	if method == "POST" {
		h += "Content-Length: 0\r\n"
	}
	return []byte(h + "\r\n")
}

func BenchmarkHeader(b *testing.B) {
	head := benchHead("POST", "/posts/123456/like", true)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if header(head, "content-length:") == nil {
			b.Fatal("not found")
		}
	}
}

func BenchmarkParseFeed(b *testing.B) {
	s, head := benchServer(), benchHead("GET", "/feed", false)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if r := s.parse(head, nil); r.kind != reqFeed {
			b.Fatal(r.kind)
		}
	}
}

func BenchmarkParsePost(b *testing.B) {
	s, head := benchServer(), benchHead("GET", "/posts/123456", false)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if r := s.parse(head, nil); r.kind != reqPost {
			b.Fatal(r.kind)
		}
	}
}

// The token check alone, and the whole parse of a like (which is mostly the token check).
func BenchmarkAuth(b *testing.B) {
	s, head := benchServer(), benchHead("POST", "/posts/123456/like", true)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if _, _, msg := s.auth(head); msg != "" {
			b.Fatal(msg)
		}
	}
}

func BenchmarkParseLike(b *testing.B) {
	s, head := benchServer(), benchHead("POST", "/posts/123456/like", true)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if r := s.parse(head, nil); r.kind != reqLike {
			b.Fatal(r.kind)
		}
	}
}

func BenchmarkParseCreate(b *testing.B) {
	s, head := benchServer(), benchHead("POST", "/posts", true)
	body := []byte(`{"body":"a post of ordinary length, about as long as the load test writes them"}`)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		r := s.parse(head, body)
		if r.kind != reqCreate {
			b.Fatal(r.kind)
		}
		s.discard(&r)
	}
}

// The two JSON string writers on a typical post body: Go's (appendString) and C's.
var benchText = bytes.Repeat([]byte("Just shipped the thing we talked about. "), 5)

func BenchmarkJSONStringGo(b *testing.B) {
	out := make([]byte, 0, 1024)
	b.SetBytes(int64(len(benchText)))
	for i := 0; i < b.N; i++ {
		out = appendString(out[:0], benchText)
	}
}

func BenchmarkJSONStringC(b *testing.B) {
	out := make([]byte, 1024)
	b.SetBytes(int64(len(benchText)))
	for i := 0; i < b.N; i++ {
		dbJSONString(out, benchText)
	}
}

// What one crossing into C costs with nothing done on the other side (db_backfills reads one number).
func BenchmarkCgoCall(b *testing.B) {
	for i := 0; i < b.N; i++ {
		dbBackfills()
	}
}

var benchDBOpen bool

func benchDB(b *testing.B) (s *server, maxID int64) {
	path := os.Getenv("BENCH_DB")
	if path == "" {
		b.Skip("BENCH_DB not set")
	}
	if !benchDBOpen {
		if err := dbOpen(path); err != nil {
			b.Fatal(err)
		}
		benchDBOpen = true
		fmt.Fprintln(os.Stderr, "direct B-tree reads:", dbReadsDirect())
	}
	maxID = 100000
	if v, err := strconv.ParseInt(os.Getenv("BENCH_MAXID"), 10, 64); err == nil {
		maxID = v
	}
	// The testing package calls a benchmark several times; a transaction left open by one
	// call would make the next call's BEGIN fail.
	s = benchServer()
	b.Cleanup(func() {
		if s.inTxn {
			dbCommit()
			s.inTxn = false
		}
	})
	return s, maxID
}

// endBatch commits as the loop does at the end of a pass of 256 answers.
func (s *server) benchEndBatch(i int) {
	if i&255 == 255 && s.inTxn {
		dbCommit()
		s.inTxn = false
	}
}

func BenchmarkFeed(b *testing.B) {
	s, _ := benchDB(b)
	out := make([]byte, 0, sendBufSize)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if o := s.feed(out[:0]); len(o) < 1000 {
			b.Fatalf("short feed: %q", o)
		}
		s.benchEndBatch(i)
	}
}

// Posts in a scattered order, so the pages are not the ones just read.
func BenchmarkPost(b *testing.B) {
	s, maxID := benchDB(b)
	out := make([]byte, 0, sendBufSize)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		id := int64(uint64(i)*2654435761%uint64(maxID)) + 1
		if o := s.getPost(out[:0], id); !bytes.HasPrefix(o, []byte("HTTP/1.1 200")) {
			b.Fatalf("post %d: %q", id, o)
		}
		s.benchEndBatch(i)
	}
}

// The feed's posts, as the load test asks for them: the newest twenty.
func BenchmarkPostRecent(b *testing.B) {
	s, maxID := benchDB(b)
	out := make([]byte, 0, sendBufSize)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if o := s.getPost(out[:0], maxID-int64(i%20)); !bytes.HasPrefix(o, []byte("HTTP/1.1 200")) {
			b.Fatalf("%q", o)
		}
		s.benchEndBatch(i)
	}
}

func BenchmarkPostExists(b *testing.B) {
	_, maxID := benchDB(b)
	if !dbBegin() {
		b.Fatal(dbErr())
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if dbPostExists(maxID-int64(i%20)) != 1 {
			b.Fatal("missing")
		}
	}
	b.StopTimer()
	dbCommit()
}

// One whole request of each read kind: parse the head, read, build the response.
func BenchmarkRequestFeed(b *testing.B) {
	s, _ := benchDB(b)
	head, out := benchHead("GET", "/feed", false), make([]byte, 0, sendBufSize)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		r := s.parse(head, nil)
		s.execute(out[:0], &r)
		s.benchEndBatch(i)
	}
}

func BenchmarkRequestPost(b *testing.B) {
	s, maxID := benchDB(b)
	out := make([]byte, 0, sendBufSize)
	heads := make([][]byte, 20)
	for i := range heads {
		heads[i] = benchHead("GET", "/posts/"+strconv.FormatInt(maxID-int64(i), 10), false)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		r := s.parse(heads[i%20], nil)
		s.execute(out[:0], &r)
		s.benchEndBatch(i)
	}
}

var benchBody = []byte(`{"body":"a post of ordinary length, about as long as the load test writes them"}`)

// Under load the feed's twenty posts are ones written in the last fraction of a second and
// have no likes yet; the seed's newest twenty have several each, and counting them is a
// large part of BenchmarkFeed. benchFresh writes twenty new posts once, so the feed read
// after it is the one the load test asks for.
var benchFreshDone bool

func benchFresh(b *testing.B, s *server) {
	if benchFreshDone {
		return
	}
	head, out := benchHead("POST", "/posts", true), make([]byte, 0, sendBufSize)
	for i := 0; i < 20; i++ {
		r := s.parse(head, benchBody)
		if o := s.execute(out[:0], &r); !bytes.HasPrefix(o, []byte("HTTP/1.1 201")) {
			b.Fatalf("%q", o)
		}
	}
	if s.inTxn {
		dbCommit()
		s.inTxn = false
	}
	benchFreshDone = true
}

func BenchmarkZFeedFresh(b *testing.B) {
	s, _ := benchDB(b)
	benchFresh(b, s)
	out := make([]byte, 0, sendBufSize)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if o := s.feed(out[:0]); len(o) < 1000 {
			b.Fatalf("short feed: %q", o)
		}
		s.benchEndBatch(i)
	}
}

// A like of a post this user has not liked: token check, existence check, insert. The commit
// is shared by 256 of them here; under load by about a hundred.
func BenchmarkZRequestLike(b *testing.B) {
	s, maxID := benchDB(b)
	out := make([]byte, 0, sendBufSize)
	tok := benchToken()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		id := int64(uint64(i)*2654435761%uint64(maxID)) + 1
		head := append(append(append(s.json[:0], "POST /posts/"...), strconv.AppendInt(nil, id, 10)...), "/like HTTP/1.1\r\nHost: 10.0.0.1\r\nUser-Agent: Grafana k6/1.2.3\r\nAuthorization: Bearer "+tok+"\r\nContent-Length: 0\r\n\r\n"...)
		r := s.parse(head, nil)
		s.execute(out[:0], &r)
		s.benchEndBatch(i)
	}
}

func BenchmarkZRequestCreate(b *testing.B) {
	s, _ := benchDB(b)
	head, out := benchHead("POST", "/posts", true), make([]byte, 0, sendBufSize)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		r := s.parse(head, benchBody)
		s.execute(out[:0], &r)
		s.benchEndBatch(i)
	}
}

// The feed as the load test meets it: twenty posts just written, by twenty different authors
// scattered over the users table.
func BenchmarkZZFeedAuthors(b *testing.B) {
	s, _ := benchDB(b)
	out := make([]byte, 0, sendBufSize)
	for i := 0; i < 20; i++ {
		head := []byte("POST /posts HTTP/1.1\r\nHost: 10.0.0.1\r\nAuthorization: Bearer " + benchTokenFor(1+i*2477) + "\r\nContent-Length: 0\r\n\r\n")
		r := s.parse(head, benchBody)
		if o := s.execute(out[:0], &r); !bytes.HasPrefix(o, []byte("HTTP/1.1 201")) {
			b.Fatalf("%q", o)
		}
	}
	if s.inTxn {
		dbCommit()
		s.inTxn = false
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if o := s.feed(out[:0]); len(o) < 1000 {
			b.Fatalf("short feed: %q", o)
		}
		s.benchEndBatch(i)
	}
}

// The pieces of one post object on their own, each through one crossing into C (subtract
// BenchmarkCgoCall): a name, a date, a body, an id and a like count of zero.
func benchStr(b *testing.B, s string) {
	out, in := make([]byte, 1024), []byte(s)
	for i := 0; i < b.N; i++ {
		dbJSONString(out, in)
	}
}

func BenchmarkYStrName(b *testing.B) { benchStr(b, "user_48213") }
func BenchmarkYStrDate(b *testing.B) { benchStr(b, "2026-11-02T10:15:30.123Z") }
func BenchmarkYStrBody(b *testing.B) {
	benchStr(b, "a post of ordinary length, about as long as the load test writes them, 80 bytes.")
}

// The WAL file must receive exactly what SQLite writes: the same bytes at the same offsets in
// the same order, only in fewer calls. db.c hashes both sides; after a commit nothing is
// buffered and the hashes must be equal. Needs a database (BENCH_DB), like the benchmarks.
func TestWALWritesUnchanged(t *testing.T) {
	path := os.Getenv("BENCH_DB")
	if path == "" {
		t.Skip("BENCH_DB not set")
	}
	if !benchDBOpen {
		if err := dbOpen(path); err != nil {
			t.Fatal(err)
		}
		benchDBOpen = true
	}
	s, out, tok := benchServer(), make([]byte, 0, sendBufSize), benchToken()
	create := benchHead("POST", "/posts", true)
	dbWalWrites()
	dbWalCheck(true)
	commits := 0
	for i := 0; i < 6000; i++ {
		var r request
		if i%7 == 0 {
			r = s.parse(create, benchBody)
		} else {
			id := int64(uint64(i)*2654435761%100000) + 1
			head := append(append(append(s.json[:0], "POST /posts/"...), strconv.AppendInt(nil, id, 10)...), "/like HTTP/1.1\r\nHost: 10.0.0.1\r\nAuthorization: Bearer "+tok+"\r\nContent-Length: 0\r\n\r\n"...)
			r = s.parse(head, nil)
		}
		s.execute(out[:0], &r)
		// Commits of very different sizes: one request, a handful, and one of 2,000.
		if s.inTxn && (i < 50 || (i < 3000 && i%13 == 0) || i == 5000 || i == 5999) {
			if !dbCommit() {
				t.Fatal("commit failed: ", dbErr())
			}
			s.inTxn = false
			commits++
			if !dbWalCheck(false) {
				t.Fatalf("after commit %d (request %d): the WAL file did not receive what SQLite wrote", commits, i)
			}
		}
	}
	writes, frames := dbWalWrites()
	t.Logf("%d commits, %d frames, %d write calls on the WAL", commits, frames, writes)
	// A commit that changed nothing (every like in it already existed, on a database this
	// test has run on before) writes nothing, so write calls can be fewer than commits.
	if frames == 0 || writes >= frames {
		t.Errorf("%d write calls for %d commits and %d frames: the layer is not batching", writes, commits, frames)
	}
}
