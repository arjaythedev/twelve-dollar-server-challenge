package main

import (
	"bytes"
	"strings"
	"testing"
)

func TestFeedJSONParity(t *testing.T) {
	a := newApp(nil)
	all := make([]byte, 256)
	for i := range all {
		all[i] = byte(i)
	}
	for _, body := range [][]byte{nil, all, []byte(strings.Repeat("plain", 100)), []byte("\"\\\n你好🙂")} {
		created, author := []byte("2026-11-09 01:02:03"), []byte("a\"b\\c")
		want := a.appendPost([]byte("prefix:"), -9223372036854775808, body, created, author, 9223372036854775807)
		for capacity := 0; capacity < len(want)+32; capacity++ {
			backing := bytes.Repeat([]byte{0xa5}, capacity+32)
			out := append(backing[:0:capacity], "prefix:"...)
			got := a.appendFeedPost(out, -9223372036854775808, body, created, author, 9223372036854775807)
			if !bytes.Equal(got, want) || !bytes.Equal(backing[capacity:], bytes.Repeat([]byte{0xa5}, 32)) {
				t.Fatalf("capacity=%d body=%q", capacity, body)
			}
		}
		buf := make([]byte, 0, 8192)
		if n := testing.AllocsPerRun(100, func() {
			buf = a.appendFeedPost(buf[:0], 500000, body, created, author, 12)
		}); n != 0 {
			t.Fatalf("steady-state allocations: %g", n)
		}
	}
}

func BenchmarkFeedJSON(b *testing.B) {
	a := newApp(nil)
	for _, tc := range []struct{ name, body string }{
		{"plain100", strings.Repeat("plain", 20)},
		{"plain500", strings.Repeat("plain", 100)},
		{"escaped300", strings.Repeat("\"\\\n", 100)},
		{"unicode500", strings.Repeat("你好🙂", 50)},
	} {
		b.Run(tc.name, func(b *testing.B) {
			body := []byte(tc.body)
			created, author := []byte("2026-11-09 01:02:03"), []byte("golden_ember_1")
			out := make([]byte, 0, 32768)
			b.ReportAllocs()
			for i := 0; i < b.N; i++ {
				out = out[:0]
				for row := 0; row < 20; row++ {
					out = a.appendFeedPost(out, int64(500000-row), body, created, author, 12)
				}
			}
		})
	}
}
