package main

import (
	"bytes"
	"encoding/json"
	"os"
	"syscall"
	"testing"
)

// Put every possible short tail against an unreadable page. A word load past
// the slice boundary must fault rather than silently reading adjacent memory.
func TestAppendEscGuardPage(t *testing.T) {
	page := os.Getpagesize()
	memory, err := syscall.Mmap(-1, 0, 2*page, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANON)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Munmap(memory)
	if err := syscall.Mprotect(memory[page:], syscall.PROT_NONE); err != nil {
		t.Fatal(err)
	}
	for length := 0; length <= 65; length++ {
		raw := memory[page-length : page : page]
		for _, special := range []byte{'x', '"', '\\', 0, '\n', 0x1f} {
			for i := range raw {
				raw[i] = 'x'
			}
			if length > 0 {
				raw[length-1] = special
			}
			doc := appendEsc([]byte{'"'}, raw)
			doc = append(doc, '"')
			var decoded string
			if err := json.Unmarshal(doc, &decoded); err != nil || !bytes.Equal([]byte(decoded), raw) {
				t.Fatalf("length=%d special=%d: %q err=%v", length, special, decoded, err)
			}
		}
	}
}
