package jsoncopy

import "unsafe"

// Copy copies the JSON-safe prefix of src into disjoint dst, returning its length.
// It reads/writes at most min(len(dst), len(src)) bytes. Bytes >= 0x80 are
// preserved, matching the server's existing byte-oriented JSON escaping.
func Copy(dst, src []byte) int {
	return int(copyPlain(unsafe.SliceData(dst), unsafe.SliceData(src), uintptr(min(len(dst), len(src)))))
}

//go:noescape
func copyPlain(dst, src *byte, n uintptr) uintptr
