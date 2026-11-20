package main

import (
	"bytes"
	"time"
	"unicode/utf8"
	"unsafe"
)

// handle routes one parsed request. Responses that need no database are appended to the
// connection's output now; the others are added to the pass's batch (hDeferred). hFull means
// nothing was done because the batch or the arena is full.
func (s *server) handle(ri int, slot uint32, c *conn, r *request) int {
	p := r.path
	switch r.method {
	case mGET:
		if string(p) == "/feed" {
			if s.deferReq(ri, slot, c, kFeed, feedReserve) == nil {
				return hFull
			}
			return hDeferred
		}
		if len(p) > 7 && string(p[:7]) == "/posts/" {
			id, ok := positive(p[7:])
			if !ok {
				if bytes.IndexByte(p[7:], '/') >= 0 {
					s.errorNow(ri, slot, c, 404, "not found")
				} else {
					s.errorNow(ri, slot, c, 400, "invalid post id")
				}
				return hDone
			}
			q := s.deferReq(ri, slot, c, kPost, smallReserve)
			if q == nil {
				return hFull
			}
			q.id = id
			return hDeferred
		}
		if string(p) == "/health" {
			q := s.deferReq(ri, slot, c, kHealth, smallReserve)
			if q == nil {
				return hFull
			}
			q.id = int64(time.Since(s.started) / time.Second)
			return hDeferred
		}
	case mPOST:
		if string(p) == "/posts" {
			return s.create(ri, slot, c, r)
		}
		if len(p) > 12 && string(p[:7]) == "/posts/" && string(p[len(p)-5:]) == "/like" {
			mid := p[7 : len(p)-5]
			if bytes.IndexByte(mid, '/') < 0 {
				return s.like(ri, slot, c, r, mid)
			}
		}
	}
	s.errorNow(ri, slot, c, 404, "not found")
	return hDone
}

// auth verifies the token; on failure the 401 is written and ok is false.
func (s *server) auth(ri int, slot uint32, c *conn, r *request) (int64, []byte, bool) {
	st, uid, name := s.jwt.verify(r.auth, r.hasAuth, s.now)
	switch st {
	case authOK:
		return uid, name, true
	case authMissing:
		s.errorNow(ri, slot, c, 401, "missing bearer token")
	case authInvalid:
		s.errorNow(ri, slot, c, 401, "invalid or expired token")
	default:
		s.errorNow(ri, slot, c, 401, "invalid token payload")
	}
	return 0, nil, false
}

func (s *server) like(ri int, slot uint32, c *conn, r *request, rawID []byte) int {
	uid, _, ok := s.auth(ri, slot, c, r)
	if !ok {
		return hDone
	}
	id, ok := positive(rawID)
	if !ok {
		s.errorNow(ri, slot, c, 400, "invalid post id")
		return hDone
	}
	q := s.deferReq(ri, slot, c, kLike, smallReserve)
	if q == nil {
		return hFull
	}
	q.id = id
	q.uid = uid
	return hDeferred
}

var bodyKey = []string{"body"}

// jsSpace reports JavaScript's String.prototype.trim whitespace.
func jsSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
		return true
	}
	return r >= 0x2000 && r <= 0x200a
}

func trimJS(s []byte) []byte {
	for len(s) > 0 {
		if s[0] < 0x80 {
			if !jsSpace(rune(s[0])) {
				break
			}
			s = s[1:]
			continue
		}
		r, n := utf8.DecodeRune(s)
		if !jsSpace(r) {
			break
		}
		s = s[n:]
	}
	for len(s) > 0 {
		c := s[len(s)-1]
		if c < 0x80 {
			if !jsSpace(rune(c)) {
				break
			}
			s = s[:len(s)-1]
			continue
		}
		r, n := utf8.DecodeLastRune(s)
		if !jsSpace(r) {
			break
		}
		s = s[:len(s)-n]
	}
	return s
}

func (s *server) create(ri int, slot uint32, c *conn, r *request) int {
	uid, name, ok := s.auth(ri, slot, c, r)
	if !ok {
		return hDone
	}
	var ms [1]member
	valid, isObj := object(r.body, bodyKey, ms[:], s.body[:0])
	if !valid {
		s.errorNow(ri, slot, c, 400, "malformed JSON body")
		return hDone
	}
	m := ms[0]
	if !isObj || !m.found || m.kind != jStr {
		s.errorNow(ri, slot, c, 400, "body is required")
		return hDone
	}
	txt := m.val
	if m.esc {
		s.body = unescape(m.val, s.body[:0])
		txt = s.body
	}
	txt = trimJS(txt)
	if len(txt) == 0 {
		s.errorNow(ri, slot, c, 400, "body is required")
		return hDone
	}
	if len(txt) > 500 && utf8.RuneCount(txt) > 500 {
		s.errorNow(ri, slot, c, 400, "body must be at most 500 characters")
		return hDone
	}
	// The body and the name must outlive the receive buffer: the batch reads them at the end
	// of the pass.
	kb := s.keep(txt)
	kn := s.keep(name)
	if kb == nil || kn == nil {
		return hFull
	}
	q := s.deferReq(ri, slot, c, kInsert, smallReserve)
	if q == nil {
		return hFull
	}
	q.uid = uid
	q.body = unsafe.Pointer(unsafe.SliceData(kb))
	q.nbody = int32(len(kb))
	q.name = unsafe.Pointer(unsafe.SliceData(kn))
	q.nname = int32(len(kn))
	return hDeferred
}
