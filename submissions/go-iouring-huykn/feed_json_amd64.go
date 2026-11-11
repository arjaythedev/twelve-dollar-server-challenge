//go:build amd64 && feed_sse2

package main

import (
	"go-iouring-huykn/internal/jsoncopy"
	"slices"
)

// Reserve only the unescaped length. Worst-case feed reservations exceed the
// output pool's retention limit even for ordinary 500-byte ASCII posts.
func appendFeedEsc(dst, src []byte) []byte {
	dst = slices.Grow(dst, len(src))
	at := len(dst)
	n := jsoncopy.Copy(dst[at:at+len(src)], src)
	dst = dst[:at+n]
	if n == len(src) {
		return dst
	}
	return appendEscSlow(dst, src[n:])
}

// Keep detail/create on their independently measured append-based writer.
func (a *app) appendFeedPost(dst []byte, id int64, body, created, author []byte, likes int64) []byte {
	dst = append(dst, `{"id":`...)
	dst = appendInt(dst, id)
	dst = append(dst, `,"body":"`...)
	dst = appendFeedEsc(dst, body)
	dst = append(dst, `","created_at":"`...)
	dst = appendFeedEsc(dst, created)
	dst = append(dst, `","author":"`...)
	dst = appendFeedEsc(dst, author)
	dst = append(dst, `","like_count":`...)
	dst = appendInt(dst, likes)
	return append(dst, '}')
}
