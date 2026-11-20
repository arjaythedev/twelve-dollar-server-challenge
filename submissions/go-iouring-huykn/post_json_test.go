package main

import (
	"bytes"
	"encoding/json"
	"os"
	"runtime"
	"strings"
	"testing"
)

// Ordinary maximum-length ASCII posts must keep using a reusable output buffer.
func TestFeedPoolReuse(t *testing.T) {
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
	for i := 0; i < 20; i++ {
		if _, _, ok := dbInsertPost(1, bytes.Repeat([]byte{'x'}, 500)); !ok {
			t.Fatal("insert fixture")
		}
	}
	if !dbCommit() {
		t.Fatal("commit fixture")
	}
	s := server{app: newApp(nil)}
	request := func() {
		i := s.takeOut()
		s.pool[i-1] = s.app.feed(s.buf(i)[:0])
		if !dbCommit() || !bytes.HasPrefix(s.buf(i), []byte("HTTP/1.1 200 ")) {
			t.Fatal("feed failed")
		}
		s.give(i)
	}
	if n := testing.AllocsPerRun(10, request); n != 0 {
		t.Fatalf("feed pool allocates after warmup: %g", n)
	}
}

func TestPostJSONGrowth(t *testing.T) {
	a := newApp(nil)
	var controls strings.Builder
	for r := rune(0); r < 32; r++ {
		controls.WriteRune(r)
	}
	for _, body := range []string{"", strings.Repeat("plain", 100), controls.String(), "quote\" slash\\ 你好🙂"} {
		want := expectedPost{-9223372036854775808, body, "2026-11-09 01:02:03", "author", 9223372036854775807}
		var encoded bytes.Buffer
		encoder := json.NewEncoder(&encoded)
		encoder.SetEscapeHTML(false)
		if err := encoder.Encode(want); err != nil {
			t.Fatal(err)
		}
		for _, capacity := range []int{0, 7, 32, 8192} {
			backing := bytes.Repeat([]byte{0xa5}, capacity+32)
			out := backing[:0:capacity]
			out = append(out, "prefix:"...)
			got := a.appendPost(out, want.ID, []byte(body), []byte(want.Created), []byte(want.Author), want.Likes)
			if !bytes.Equal(got, append([]byte("prefix:"), bytes.TrimSuffix(encoded.Bytes(), []byte{'\n'})...)) {
				t.Fatalf("capacity %d: %q", capacity, got)
			}
			if !bytes.Equal(backing[capacity:], bytes.Repeat([]byte{0xa5}, 32)) {
				t.Fatal("write beyond caller capacity")
			}
		}
	}
}

func BenchmarkPostJSON(b *testing.B) {
	a := newApp(nil)
	for _, tc := range []struct{ name, body string }{
		{"plain100", strings.Repeat("plain", 20)},
		{"plain500", strings.Repeat("plain", 100)},
		{"escaped300", strings.Repeat("\"\\\n", 100)},
	} {
		b.Run(tc.name, func(b *testing.B) {
			out := make([]byte, 0, 8192)
			b.ReportAllocs()
			for i := 0; i < b.N; i++ {
				out = a.appendPost(out[:0], 500000, []byte(tc.body), []byte("2026-11-09 01:02:03"), []byte("golden_ember_1"), 12)
			}
		})
	}
}
