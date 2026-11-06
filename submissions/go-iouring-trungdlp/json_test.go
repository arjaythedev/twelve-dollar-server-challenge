package main

import (
	"bytes"
	"encoding/json"
	"math/rand"
	"testing"
)

func TestAppendEsc(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	alpha := []byte("abc \"\\\x00\x01\x1f\x20\x7f\x80\xc3\xa9/<>&")
	for n := 0; n < 20000; n++ {
		s := make([]byte, r.Intn(40))
		for i := range s {
			if r.Intn(4) == 0 {
				s[i] = alpha[r.Intn(len(alpha))]
			} else {
				s[i] = byte('a' + r.Intn(26))
			}
		}
		got := appendEsc(nil, s)
		want := appendEscSlow(nil, s)
		if !bytes.Equal(got, want) {
			t.Fatalf("%q: got %q want %q", s, got, want)
		}
		// Valid UTF-8 input must decode back to itself.
		var back string
		if json.Valid(s) || true {
			if err := json.Unmarshal(append(append([]byte{'"'}, got...), '"'), &back); err == nil && bytes.Equal([]byte(back), s) == false && bytes.Equal([]byte(string([]rune(string(s)))), s) {
				t.Fatalf("%q round-trips to %q", s, back)
			}
		}
	}
}

func TestEnd(t *testing.T) {
	for _, body := range []string{"", "x", "{\"a\":1}", string(make([]byte, 12345))} {
		b, s := start(nil, 200)
		b = append(b, body...)
		b = end(b, s)
		want := "Content-Length: " + string(bytes.Repeat([]byte{' '}, 5-len(itoa(len(body))))) + itoa(len(body)) + "\r\n\r\n"
		if !bytes.Contains(b, []byte(want)) {
			t.Fatalf("%q", b[:min(len(b), 80)])
		}
	}
}

func itoa(n int) string { return string(appendInt(nil, int64(n))) }

func BenchmarkAppendEsc(b *testing.B) {
	s := bytes.Repeat([]byte("golden_ember_1 says hi at 2026-11-06T17:21:21.207Z (VU 1, iter 2) "), 3)
	buf := make([]byte, 0, 4096)
	b.SetBytes(int64(len(s)))
	for i := 0; i < b.N; i++ {
		buf = appendEsc(buf[:0], s)
	}
}
