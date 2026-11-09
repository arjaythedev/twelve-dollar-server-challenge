//go:build !amd64

package jsoncopy

// Copy copies only the JSON-safe prefix into disjoint dst, bounded by both slices.
func Copy(dst, src []byte) int {
	for i, c := range src[:min(len(dst), len(src))] {
		if c < 0x20 || c == '"' || c == '\\' {
			return i
		}
		dst[i] = c
	}
	return min(len(dst), len(src))
}
