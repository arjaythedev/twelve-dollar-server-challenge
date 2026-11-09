package jsoncopy

import (
	"os"
	"syscall"
	"testing"
)

// Both input and output end at inaccessible pages. Include short, unaligned
// vector tails and every possible byte at each edge of the last vector.
func TestCopyGuardPages(t *testing.T) {
	page := os.Getpagesize()
	allocate := func() []byte {
		m, err := syscall.Mmap(-1, 0, 2*page, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE|syscall.MAP_ANON)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { syscall.Munmap(m) })
		if err := syscall.Mprotect(m[page:], syscall.PROT_NONE); err != nil {
			t.Fatal(err)
		}
		return m[:page:page]
	}
	source, destination := allocate(), allocate()
	for length := 0; length <= 80; length++ {
		checkCopyGuardPageLength(t, source[page-length:], destination[page-length:], length)
	}
}

func checkCopyGuardPageLength(t *testing.T, src, dst []byte, length int) {
	t.Helper()
	for special := 0; special < 256; special++ {
		for i := range src {
			src[i] = 'x'
		}
		want := length
		if length > 0 {
			src[length-1] = byte(special)
			if special < 32 || special == '"' || special == '\\' {
				want--
			}
		}
		if n := Copy(dst, src); n != want {
			t.Fatalf("length=%d byte=%d copied=%d want=%d", length, special, n, want)
		}
	}
}
