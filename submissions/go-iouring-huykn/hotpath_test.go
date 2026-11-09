package main

import (
	"bytes"
	"encoding/json"
	"strconv"
	"testing"
)

func TestContentLengthBounds(t *testing.T) {
	for _, raw := range []string{"", "0", "000000000", "1", "500", "1048576", "1048577", "999999999", "1000000000", "-1", "+1", "1x", " 1", "1 ", "1\n", "１２"} {
		want, err := strconv.ParseUint(raw, 10, 32)
		valid := err == nil && len(raw) <= 9 && want <= maxBody
		got, ok := contentBytes([]byte(raw))
		if ok != valid || (ok && uint64(got) != want) {
			t.Fatalf("length %q: got %d/%v, want %d/%v", raw, got, ok, want, valid)
		}
	}
}

func TestEscapedLongPrefix(t *testing.T) {
	for _, size := range []int{33, 127, 500, 2048} {
		for at := 0; at < size; at++ {
			for _, special := range []byte{'"', '\\', '\n', 0} {
				raw := bytes.Repeat([]byte{'x'}, size)
				raw[at] = special
				doc := append(appendEsc([]byte{'"'}, raw), '"')
				var got string
				if err := json.Unmarshal(doc, &got); err != nil || got != string(raw) {
					t.Fatalf("size=%d at=%d special=%d: %v", size, at, special, err)
				}
			}
		}
	}
}

func BenchmarkEscapePosition(b *testing.B) {
	for _, at := range []int{-1, 0, 250, 499} {
		b.Run(strconv.Itoa(at), func(b *testing.B) {
			raw := bytes.Repeat([]byte{'x'}, 500)
			if at >= 0 {
				raw[at] = '\n'
			}
			out := make([]byte, 0, 4096)
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				out = appendEsc(out[:0], raw)
			}
		})
	}
}
