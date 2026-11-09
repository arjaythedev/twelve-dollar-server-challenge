//go:build !amd64 || !feed_sse2

package main

func (a *app) appendFeedPost(dst []byte, id int64, body, created, author []byte, likes int64) []byte {
	return a.appendPost(dst, id, body, created, author, likes)
}
