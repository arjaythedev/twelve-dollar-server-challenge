package jsoncopy

import (
	"bytes"
	"testing"
)

func TestCopyBoundaries(t *testing.T) {
	for alignment := 0; alignment < 16; alignment++ {
		for length := 0; length <= 80; length++ {
			checkCopyBoundaryBytes(t, alignment, length)
		}
	}
}

func checkCopyBoundaryBytes(t *testing.T, alignment, length int) {
	t.Helper()
	for special := 0; special < 256; special++ {
		for _, position := range []int{0, length / 2, length - 1} {
			checkCopyBoundary(t, alignment, length, special, position)
		}
	}
}

func checkCopyBoundary(t *testing.T, alignment, length, special, position int) {
	t.Helper()
	source := bytes.Repeat([]byte{'x'}, alignment+length)
	src := source[alignment:]
	if position >= 0 && position < length {
		src[position] = byte(special)
	}
	want := cleanPrefixLength(src)
	for _, capacity := range []int{length / 2, length} {
		checkCopyBoundaryCapacity(t, alignment, length, special, position, capacity, src, want)
	}
}

func checkCopyBoundaryCapacity(t *testing.T, alignment, length, special, position, capacity int, src []byte, prefix int) {
	t.Helper()
	backing := bytes.Repeat([]byte{0xa5}, alignment+length+16)
	dst := backing[alignment : alignment+capacity : alignment+capacity]
	n := Copy(dst, src)
	want := min(prefix, capacity)
	if n != want || !bytes.Equal(dst[:n], src[:n]) {
		t.Fatalf("align=%d len=%d byte=%d pos=%d cap=%d copied=%d want=%d", alignment, length, special, position, capacity, n, want)
	}
	for i, c := range backing {
		if (i < alignment || i >= alignment+n) && c != 0xa5 {
			t.Fatalf("write outside safe prefix at %d", i)
		}
	}
}

func cleanPrefixLength(src []byte) int {
	for i, c := range src {
		if c < 32 || c == '"' || c == '\\' {
			return i
		}
	}
	return len(src)
}

func FuzzCopy(f *testing.F) {
	f.Add([]byte("plain text with a \" quote"), uint8(19))
	f.Add([]byte{0, 31, 32, 128, 255}, uint8(5))
	f.Fuzz(func(t *testing.T, src []byte, capacity uint8) {
		dst := bytes.Repeat([]byte{0xa5}, int(capacity)+16)
		limit := min(len(src), int(capacity))
		want := cleanPrefixLength(src[:limit])
		n := Copy(dst[:capacity:capacity], src)
		if n != want || !bytes.Equal(dst[:n], src[:n]) || !bytes.Equal(dst[n:], bytes.Repeat([]byte{0xa5}, len(dst)-n)) {
			t.Fatalf("copied=%d want=%d", n, want)
		}
	})
}
