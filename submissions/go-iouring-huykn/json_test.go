package main

import (
	"bytes"
	"testing"
)

func TestAppendEsc(t *testing.T) {
	for _, input := range []string{"plain", "quote: \" slash: \\", "\x00\x01\x1f\n\t\r\b\f", "café ✓ /<>&"} {
		raw := []byte(input)
		escaped := appendEsc(nil, raw)
		if !bytes.Equal(escaped, appendEscSlow(nil, raw)) {
			t.Fatalf("escaping mismatch for %q", input)
		}
		var members [1]member
		doc := append(append([]byte(`{"body":"`), escaped...), []byte(`"}`)...)
		valid, obj := object(doc, []string{"body"}, members[:], nil)
		if !valid || !obj || members[0].kind != jStr {
			t.Fatalf("invalid escaped object: %s", doc)
		}
		decoded := members[0].val
		if members[0].esc {
			decoded = unescape(decoded, nil)
		}
		if !bytes.Equal(decoded, raw) {
			t.Fatalf("round trip: %q != %q", decoded, raw)
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
