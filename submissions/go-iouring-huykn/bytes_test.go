package main

import (
	"bytes"
	"encoding/json"
	"math"
	"strconv"
	"testing"
)

func TestAppendInt(t *testing.T) {
	for _, n := range []int64{math.MinInt64, -100, -99, -10, -1, 0, 9, 10, 99, 100, math.MaxInt64} {
		got := appendInt([]byte("prefix:"), n)
		want := strconv.AppendInt([]byte("prefix:"), n, 10)
		if !bytes.Equal(got, want) {
			t.Fatalf("integer %d: %s", n, got)
		}
	}
}

func TestAppendEsc(t *testing.T) {
	for _, input := range []string{"plain", "quote: \" slash: \\", "\x00\x01\x1f\n\t\r\b\f", "café ✓ /<>&"} {
		raw := []byte(input)
		escaped := appendEsc(nil, raw)
		var body byteValue
		doc := append(append([]byte(`{"body":"`), escaped...), []byte(`"}`)...)
		valid, obj := walkObject(doc, nil, func(key []byte, value byteValue) {
			if string(key) == "body" {
				body = value
			}
		})
		if !valid || !obj || body.kind != byteString {
			t.Fatalf("invalid escaped object: %s", doc)
		}
		decoded := body.raw
		if body.escaped {
			decoded = unescape(decoded, nil)
		}
		if !bytes.Equal(decoded, raw) {
			t.Fatalf("round trip: %q != %q", decoded, raw)
		}
	}
}

func TestAppendEscWordBoundaries(t *testing.T) {
	for alignment := 0; alignment < 8; alignment++ {
		for length := 0; length <= 65; length++ {
			for _, special := range []byte{'x', '"', '\\', 0, '\n', 0x1f} {
				storage := bytes.Repeat([]byte{'x'}, alignment+length)
				raw := storage[alignment:]
				if length > 0 {
					raw[length-1] = special
				}
				doc := appendEsc([]byte{'"'}, raw)
				doc = append(doc, '"')
				var decoded string
				if err := json.Unmarshal(doc, &decoded); err != nil || decoded != string(raw) {
					t.Fatalf("alignment=%d length=%d special=%d: %q err=%v", alignment, length, special, decoded, err)
				}
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
