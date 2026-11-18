//go:build linux

package main

import (
	"bytes"
	"math/rand"
	"testing"
)

// header must find what the plain byte-by-byte search finds, on any request head.
func TestHeader(t *testing.T) {
	slow := func(head []byte, name string) []byte {
		for i := 0; i+1+len(name) <= len(head); i++ {
			if head[i] != '\n' {
				continue
			}
			j := 0
			for j < len(name) && head[i+1+j]|0x20 == name[j] {
				j++
			}
			if j < len(name) {
				continue
			}
			v := head[i+1+j:]
			if end := bytes.IndexByte(v, '\r'); end >= 0 {
				v = v[:end]
			}
			return bytes.TrimSpace(v)
		}
		return nil
	}
	pieces := []string{
		"GET /feed HTTP/1.1", "POST /posts HTTP/1.1", "\r\n", "\n", "\r", "Host: x", "Content-Length: 17", "content-length:5",
		"CONTENT-LENGTH:  42  ", "Authorization: Bearer abc.def.ghi", "authorization:Bearer x", "X-Content-Length: 9",
		"Content-Length", "Content-Length:", "Authorization", ": ", "content-lengt", "h: 3", " ", "\t", "Accept: */*", "authorization: ",
	}
	r := rand.New(rand.NewSource(2))
	for n := 0; n < 200000; n++ {
		var head []byte
		for k := r.Intn(12); k > 0; k-- {
			head = append(head, pieces[r.Intn(len(pieces))]...)
		}
		for _, name := range []string{"content-length:", "authorization:"} {
			if got, want := header(head, name), slow(head, name); !bytes.Equal(got, want) || (got == nil) != (want == nil) {
				t.Fatalf("%q, %s:\n got %q\nwant %q", head, name, got, want)
			}
		}
	}
}
