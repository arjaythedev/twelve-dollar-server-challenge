// Feed API: one epoll loop on one locked OS thread, SQLite in-process through cgo, group commit per
// loop iteration. See README.md. Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT.
package main

/*
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

// Small helpers so each statement costs one or two cgo calls instead of one per column.
typedef struct {
  long long id, likes;
  const unsigned char *body, *created, *author;
  int body_n, created_n, author_n;
} row_t;

static void fill(sqlite3_stmt *s, row_t *r) {
  r->id = sqlite3_column_int64(s, 0);
  r->body = sqlite3_column_text(s, 1); r->body_n = sqlite3_column_bytes(s, 1);
  r->created = sqlite3_column_text(s, 2); r->created_n = sqlite3_column_bytes(s, 2);
  r->author = sqlite3_column_text(s, 3); r->author_n = sqlite3_column_bytes(s, 3);
  r->likes = sqlite3_column_int64(s, 4);
}
// Steps once; on a row, fills r with the columns (text stays valid until the next step or reset).
static int step_fill(sqlite3_stmt *s, row_t *r) {
  int rc = sqlite3_step(s);
  if (rc == SQLITE_ROW) fill(s, r);
  return rc;
}
static int post_get(sqlite3_stmt *s, long long id, row_t *r) {
  sqlite3_bind_int64(s, 1, id);
  int rc = sqlite3_step(s);
  if (rc == SQLITE_ROW) fill(s, r);
  return rc;
}
static int step_reset(sqlite3_stmt *s) { int rc = sqlite3_step(s); sqlite3_reset(s); return rc; }
static int like_step(sqlite3 *db, sqlite3_stmt *s, long long uid, long long id, int *changes) {
  sqlite3_bind_int64(s, 1, uid); sqlite3_bind_int64(s, 2, id);
  int rc = sqlite3_step(s); sqlite3_reset(s);
  *changes = sqlite3_changes(db);
  return rc;
}
static int exists_step(sqlite3_stmt *s, long long id) { sqlite3_bind_int64(s, 1, id); return step_reset(s); }
// Insert with RETURNING, run to completion. Returns SQLITE_ROW on success with id/created_at filled in.
char created_buf[64];
static int ins_step(sqlite3_stmt *s, long long uid, const char *body, int n, long long *id, int *created_n) {
  sqlite3_bind_int64(s, 1, uid); sqlite3_bind_text(s, 2, body, n, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  if (rc == SQLITE_ROW) {
    *id = sqlite3_column_int64(s, 0);
    int cn = sqlite3_column_bytes(s, 1); if (cn > 63) cn = 63;
    memcpy(created_buf, sqlite3_column_text(s, 1), cn); *created_n = cn;
    rc = sqlite3_step(s);
    if (rc == SQLITE_DONE) rc = SQLITE_ROW;
  }
  sqlite3_reset(s); sqlite3_bind_null(s, 2);  // don't keep a pointer to Go memory
  return rc;
}
*/
import "C"

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"hash"
	"net"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"
	"strconv"
	"syscall"
	"time"
	"unsafe"
)

const (
	maxFD    = 65536
	rbufSize = 64 * 1024
	maxReq   = 64 * 1024
	batchOut = 4 << 20

	sqliteRow  = 100
	sqliteDone = 101
	sqliteBusy = 5

	soReusePort = 0xf // SO_REUSEPORT (missing from package syscall)
)

type conn struct {
	in, out []byte // partial request / unsent response bytes (both rare)
	outoff  int
	open    bool
	closing bool
}

type seg struct{ fd, off, n int }

var (
	conns  [maxFD]conn
	epfd   int
	nconns int

	db                                             *C.sqlite3
	stFeed, stPost, stIns, stLike, stExists, stOne *C.sqlite3_stmt
	stBegin, stBeginImm, stCommit, stRollback      *C.sqlite3_stmt
	row                                            C.row_t
	started, nowS                                  int64

	arena                         = make([]byte, 0, batchOut)
	segs                          = make([]seg, 0, 1024)
	deferred                      = make([]int, 0, 64)
	arenaFull                     bool
	inTxn, txnWrites, batchFailed bool

	mac      hash.Hash
	rbuf     [rbufSize]byte
	txt      [maxReq]byte
	unameBuf [1024]byte
	hdrBuf   [512]byte
	plBuf    [2048]byte
	sigBuf   [48]byte
	macBuf   [32]byte
	b64      = base64.RawURLEncoding
)

// ---------- output arena: one per loop iteration, sent after COMMIT ----------
func puts(s string) { arena = append(arena, s...) }
func put(b []byte)  { arena = append(arena, b...) }
func putu(v int64)  { arena = strconv.AppendInt(arena, v, 10) }

const hexd = "0123456789abcdef"

func putjs(s []byte) { // JSON string contents, escaped
	run := 0
	for i, c := range s {
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		arena = append(arena, s[run:i]...)
		run = i + 1
		switch c {
		case '"':
			puts(`\"`)
		case '\\':
			puts(`\\`)
		case '\n':
			puts(`\n`)
		case '\r':
			puts(`\r`)
		case '\t':
			puts(`\t`)
		default:
			arena = append(arena, '\\', 'u', '0', '0', hexd[c>>4], hexd[c&15])
		}
	}
	arena = append(arena, s[run:]...)
}

var bodyStart, clPos int

func beginResp(status int, close bool) {
	switch status {
	case 200:
		puts("HTTP/1.1 200 OK\r\n")
	case 201:
		puts("HTTP/1.1 201 Created\r\n")
	case 400:
		puts("HTTP/1.1 400 Bad Request\r\n")
	case 401:
		puts("HTTP/1.1 401 Unauthorized\r\n")
	case 404:
		puts("HTTP/1.1 404 Not Found\r\n")
	case 503:
		puts("HTTP/1.1 503 Service Unavailable\r\n")
	default:
		puts("HTTP/1.1 500 Internal Server Error\r\n")
	}
	if close {
		puts("Connection: close\r\n")
	}
	puts("Content-Type: application/json\r\nContent-Length:       \r\n\r\n")
	clPos = len(arena) - 10
	bodyStart = len(arena)
}

func finish() { // fills the blank Content-Length, right-aligned
	n := len(arena) - bodyStart
	p := clPos + 6
	for {
		p--
		arena[p] = byte('0' + n%10)
		n /= 10
		if n == 0 {
			break
		}
	}
}

func errResp(status int, msg string, close bool) {
	beginResp(status, close)
	puts(`{"error":"`)
	puts(msg)
	puts(`"}`)
	finish()
}

// ---------- JSON (strict, RFC 8259) ----------
func skipWS(s []byte, i int) int {
	for i < len(s) && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
		i++
	}
	return i
}

func hexv(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

func utf8Len(s []byte, i int) int { // length of one valid UTF-8 sequence at s[i], 0 if invalid
	c := s[i]
	rem := len(s) - i
	cont := func(k int) bool { return s[i+k]&0xc0 == 0x80 }
	switch {
	case c < 0x80:
		return 1
	case c >= 0xc2 && c <= 0xdf:
		if rem >= 2 && cont(1) {
			return 2
		}
	case c >= 0xe0 && c <= 0xef:
		if rem < 3 || !cont(1) || !cont(2) || (c == 0xe0 && s[i+1] < 0xa0) || (c == 0xed && s[i+1] > 0x9f) {
			return 0
		}
		return 3
	case c >= 0xf0 && c <= 0xf4:
		if rem < 4 || !cont(1) || !cont(2) || !cont(3) || (c == 0xf0 && s[i+1] < 0x90) || (c == 0xf4 && s[i+1] > 0x8f) {
			return 0
		}
		return 4
	}
	return 0
}

// jstr parses the string whose opening quote is at s[i], decoding UTF-8 into out when out is non-nil.
// Returns the index after the closing quote and the decoded length (-1 syntax error, -2 doesn't fit).
func jstr(s []byte, i int, out []byte) (int, int) {
	p, o, over := i+1, 0, false
	// Past cap, decoding stops but the string is still validated to its closing quote.
	emit := func(b ...byte) {
		if out != nil {
			if o+len(b) > len(out) {
				out, over = nil, true
			} else {
				copy(out[o:], b)
			}
		}
		o += len(b)
	}
	for p < len(s) {
		c := s[p]
		if c == '"' {
			if over {
				return p + 1, -2
			}
			return p + 1, o
		}
		if c < 0x20 {
			return p, -1
		}
		if c == '\\' {
			p++
			if p >= len(s) {
				return p, -1
			}
			var ch byte
			switch s[p] {
			case '"':
				ch = '"'
			case '\\':
				ch = '\\'
			case '/':
				ch = '/'
			case 'b':
				ch = '\b'
			case 'f':
				ch = '\f'
			case 'n':
				ch = '\n'
			case 'r':
				ch = '\r'
			case 't':
				ch = '\t'
			case 'u':
				if len(s)-p < 5 {
					return p, -1
				}
				a, b, c2, d := hexv(s[p+1]), hexv(s[p+2]), hexv(s[p+3]), hexv(s[p+4])
				if a|b|c2|d < 0 {
					return p, -1
				}
				cp := uint32(a<<12 | b<<8 | c2<<4 | d)
				p += 4
				if cp >= 0xd800 && cp <= 0xdbff && len(s)-p >= 7 && s[p+1] == '\\' && s[p+2] == 'u' {
					a2, b2, c3, d2 := hexv(s[p+3]), hexv(s[p+4]), hexv(s[p+5]), hexv(s[p+6])
					if a2|b2|c3|d2 < 0 {
						return p, -1
					}
					lo := uint32(a2<<12 | b2<<8 | c3<<4 | d2)
					if lo >= 0xdc00 && lo <= 0xdfff {
						cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00)
						p += 6
					}
				}
				if cp >= 0xd800 && cp <= 0xdfff {
					cp = 0xfffd // lone surrogate: U+FFFD, as JS TextEncoder does
				}
				switch {
				case cp < 0x80:
					emit(byte(cp))
				case cp < 0x800:
					emit(byte(0xc0|cp>>6), byte(0x80|cp&63))
				case cp < 0x10000:
					emit(byte(0xe0|cp>>12), byte(0x80|cp>>6&63), byte(0x80|cp&63))
				default:
					emit(byte(0xf0|cp>>18), byte(0x80|cp>>12&63), byte(0x80|cp>>6&63), byte(0x80|cp&63))
				}
				p++
				continue
			default:
				return p, -1
			}
			emit(ch)
			p++
			continue
		}
		l := utf8Len(s, p)
		if l == 0 {
			return p, -1
		}
		if out != nil {
			if o+l > len(out) {
				out, over = nil, true
			} else {
				copy(out[o:], s[p:p+l])
			}
		}
		o += l
		p += l
	}
	return p, -1
}

func jnumber(s []byte, p int) int { // index after a valid number at p, or -1
	digits := func(p int) int {
		for p < len(s) && s[p] >= '0' && s[p] <= '9' {
			p++
		}
		return p
	}
	if p < len(s) && s[p] == '-' {
		p++
	}
	if p >= len(s) {
		return -1
	}
	if s[p] == '0' {
		p++
	} else if s[p] >= '1' && s[p] <= '9' {
		p = digits(p)
	} else {
		return -1
	}
	if p < len(s) && s[p] == '.' {
		p++
		if p >= len(s) || s[p] < '0' || s[p] > '9' {
			return -1
		}
		p = digits(p)
	}
	if p < len(s) && (s[p] == 'e' || s[p] == 'E') {
		p++
		if p < len(s) && (s[p] == '+' || s[p] == '-') {
			p++
		}
		if p >= len(s) || s[p] < '0' || s[p] > '9' {
			return -1
		}
		p = digits(p)
	}
	return p
}

func jlit(s []byte, p int, lit string) int {
	if len(s)-p < len(lit) || string(s[p:p+len(lit)]) != lit {
		return -1
	}
	return p + len(lit)
}

func jvalue(s []byte, p, depth int) int { // validates and skips one value; index after it or -1
	if depth > 64 {
		return -1
	}
	p = skipWS(s, p)
	if p >= len(s) {
		return -1
	}
	switch s[p] {
	case '"':
		q, n := jstr(s, p, nil)
		if n < 0 {
			return -1
		}
		return q
	case '{':
		p = skipWS(s, p+1)
		if p < len(s) && s[p] == '}' {
			return p + 1
		}
		for {
			p = skipWS(s, p)
			if p >= len(s) || s[p] != '"' {
				return -1
			}
			q, n := jstr(s, p, nil)
			if n < 0 {
				return -1
			}
			p = skipWS(s, q)
			if p >= len(s) || s[p] != ':' {
				return -1
			}
			if p = jvalue(s, p+1, depth+1); p < 0 {
				return -1
			}
			p = skipWS(s, p)
			if p >= len(s) {
				return -1
			}
			if s[p] == ',' {
				p++
				continue
			}
			if s[p] == '}' {
				return p + 1
			}
			return -1
		}
	case '[':
		p = skipWS(s, p+1)
		if p < len(s) && s[p] == ']' {
			return p + 1
		}
		for {
			if p = jvalue(s, p, depth+1); p < 0 {
				return -1
			}
			p = skipWS(s, p)
			if p >= len(s) {
				return -1
			}
			if s[p] == ',' {
				p++
				continue
			}
			if s[p] == ']' {
				return p + 1
			}
			return -1
		}
	case 't':
		return jlit(s, p, "true")
	case 'f':
		return jlit(s, p, "false")
	case 'n':
		return jlit(s, p, "null")
	}
	return jnumber(s, p)
}

// want records the raw span of the last value of key in a top-level object (last duplicate wins).
type want struct {
	key   string
	v, ve int
	found bool
}

func jdoc(s []byte, w []want) bool {
	for i := range w {
		w[i].found = false
	}
	p := skipWS(s, 0)
	if p < len(s) && s[p] == '{' {
		p = skipWS(s, p+1)
		if p < len(s) && s[p] == '}' {
			p++
		} else {
			var kb [32]byte
			for {
				p = skipWS(s, p)
				if p >= len(s) || s[p] != '"' {
					return false
				}
				q, kl := jstr(s, p, kb[:])
				if kl == -1 {
					return false
				}
				p = skipWS(s, q)
				if p >= len(s) || s[p] != ':' {
					return false
				}
				vs := skipWS(s, p+1)
				if p = jvalue(s, vs, 1); p < 0 {
					return false
				}
				if kl >= 0 {
					for i := range w {
						if string(kb[:kl]) == w[i].key {
							w[i].v, w[i].ve, w[i].found = vs, p, true
						}
					}
				}
				p = skipWS(s, p)
				if p >= len(s) {
					return false
				}
				if s[p] == ',' {
					p++
					continue
				}
				if s[p] == '}' {
					p++
					break
				}
				return false
			}
		}
	} else if p = jvalue(s, p, 0); p < 0 {
		return false
	}
	return skipWS(s, p) == len(s)
}

// ---------- auth ----------
const (
	errBad     = "invalid or expired token"
	errPayload = "invalid token payload"
)

// auth returns the user id and username, or the message for the 401.
func auth(h []byte) (int64, []byte, string) {
	if len(h) < 7 || string(h[:7]) != "Bearer " {
		return 0, nil, "missing bearer token"
	}
	t := h[7:]
	d1 := bytes.IndexByte(t, '.')
	if d1 < 0 {
		return 0, nil, errBad
	}
	d2r := bytes.IndexByte(t[d1+1:], '.')
	if d2r < 0 {
		return 0, nil, errBad
	}
	d2 := d1 + 1 + d2r
	if bytes.IndexByte(t[d2+1:], '.') >= 0 || len(t)-d2-1 != 43 {
		return 0, nil, errBad
	}
	if n, err := b64.Decode(sigBuf[:], t[d2+1:]); err != nil || n != 32 {
		return 0, nil, errBad
	}
	mac.Reset()
	mac.Write(t[:d2])
	if !hmac.Equal(mac.Sum(macBuf[:0]), sigBuf[:32]) {
		return 0, nil, errBad
	}
	if d1 > 680 || d2-d1-1 > 2700 {
		return 0, nil, errBad
	}
	hn, err1 := b64.Decode(hdrBuf[:], t[:d1])
	pn, err2 := b64.Decode(plBuf[:], t[d1+1:d2])
	if err1 != nil || err2 != nil {
		return 0, nil, errBad
	}
	hdr, pl := hdrBuf[:hn], plBuf[:pn]
	hw := [1]want{{key: "alg"}}
	if !jdoc(hdr, hw[:]) || !hw[0].found || hdr[hw[0].v] != '"' {
		return 0, nil, errBad
	}
	var alg [8]byte
	if _, al := jstr(hdr[:hw[0].ve], hw[0].v, alg[:]); al != 5 || string(alg[:5]) != "HS256" {
		return 0, nil, errBad
	}
	pw := [4]want{{key: "exp"}, {key: "nbf"}, {key: "sub"}, {key: "username"}}
	if !jdoc(pl, pw[:]) || !pw[0].found { // exp is required
		return 0, nil, errBad
	}
	for i := 0; i < 2; i++ { // exp / nbf must be numbers, checked against now
		if !pw[i].found {
			continue
		}
		c := pl[pw[i].v]
		if c != '-' && (c < '0' || c > '9') {
			return 0, nil, errBad
		}
		num := pl[pw[i].v:pw[i].ve]
		v, err := strconv.ParseFloat(unsafe.String(&num[0], len(num)), 64)
		if err != nil && v == 0 {
			return 0, nil, errBad
		}
		if (i == 0 && float64(nowS) >= v) || (i == 1 && float64(nowS) < v) {
			return 0, nil, errBad
		}
	}
	if !pw[2].found || pl[pw[2].v] != '"' || !pw[3].found || pl[pw[3].v] != '"' {
		return 0, nil, errPayload
	}
	var sb [24]byte
	_, sl := jstr(pl[:pw[2].ve], pw[2].v, sb[:])
	if sl <= 0 || sl > 15 {
		return 0, nil, errPayload
	}
	var uid int64
	for _, c := range sb[:sl] {
		if c < '0' || c > '9' {
			return 0, nil, errPayload
		}
		uid = uid*10 + int64(c-'0')
	}
	if uid <= 0 {
		return 0, nil, errPayload
	}
	_, ul := jstr(pl[:pw[3].ve], pw[3].v, unameBuf[:])
	if ul < 0 {
		return 0, nil, errPayload
	}
	return uid, unameBuf[:ul], ""
}

// ---------- database ----------
func txn() bool { // opens the batch transaction on first use
	if inTxn {
		return true
	}
	inTxn = C.step_reset(stBegin) == sqliteDone
	return inTxn
}

// wstep runs a write in the batch transaction. With several worker processes a read snapshot can't be
// upgraded after another process committed (SQLITE_BUSY_SNAPSHOT): end the read-only part of the batch,
// take the write lock with BEGIN IMMEDIATE (waits up to busy_timeout) and retry once.
func wstep(f func() C.int) C.int {
	rc := f()
	if rc&0xff == sqliteBusy && !txnWrites {
		if inTxn {
			C.step_reset(stCommit)
			inTxn = false
		}
		if rc = C.step_reset(stBeginImm); rc != sqliteDone {
			return rc
		}
		inTxn = true
		rc = f()
	}
	if rc == sqliteRow || rc == sqliteDone {
		txnWrites = true
	}
	return rc
}

func dbFail() { // a failed statement may have rolled the whole batch back
	if inTxn && C.sqlite3_get_autocommit(db) != 0 {
		inTxn = false
		if txnWrites {
			batchFailed = true
		}
		txnWrites = false
	}
}

func cbytes(p *C.uchar, n C.int) []byte { return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n)) }

func putRow(r *C.row_t) {
	puts(`{"id":`)
	putu(int64(r.id))
	puts(`,"body":"`)
	putjs(cbytes(r.body, r.body_n))
	puts(`","created_at":"`)
	put(cbytes(r.created, r.created_n))
	puts(`","author":"`)
	putjs(cbytes(r.author, r.author_n))
	puts(`","like_count":`)
	putu(int64(r.likes))
	puts("}")
}

// ---------- request handling ----------
func parseID(s []byte) int64 { // positive decimal integer, else -1 (huge ids map to max: not found)
	if len(s) == 0 {
		return -1
	}
	var v int64
	for _, c := range s {
		if c < '0' || c > '9' {
			return -1
		}
		if v < 100000000000000000 {
			v = v*10 + int64(c-'0')
		} else {
			v = 1<<63 - 1
		}
	}
	if v > 0 {
		return v
	}
	return -1
}

func jswsAt(s []byte, p int) int { // byte length of JS String.trim whitespace at s[p], or 0
	c := s[p]
	if c == ' ' || (c >= 9 && c <= 13) {
		return 1
	}
	if len(s)-p >= 2 && c == 0xc2 && s[p+1] == 0xa0 {
		return 2
	}
	if len(s)-p >= 3 {
		x := uint32(c)<<16 | uint32(s[p+1])<<8 | uint32(s[p+2])
		if x == 0xe19a80 || (x >= 0xe28080 && x <= 0xe2808a) || x == 0xe280a8 || x == 0xe280a9 || x == 0xe280af || x == 0xe2819f || x == 0xe38080 || x == 0xefbbbf {
			return 3
		}
	}
	return 0
}

func jswsBefore(s []byte, start, e int) int {
	c := s[e-1]
	if c == ' ' || (c >= 9 && c <= 13) {
		return 1
	}
	if e-start >= 2 && s[e-2] == 0xc2 && c == 0xa0 {
		return 2
	}
	if e-start >= 3 && jswsAt(s[:e], e-3) == 3 {
		return 3
	}
	return 0
}

const internalErr = "internal server error"

func handle(m, path, authh, body []byte, close bool) {
	if q := bytes.IndexByte(path, '?'); q >= 0 {
		path = path[:q]
	}
	get, post := string(m) == "GET", string(m) == "POST"
	if get && string(path) == "/feed" {
		if !txn() {
			errResp(500, internalErr, close)
			return
		}
		mark := len(arena)
		beginResp(200, close)
		puts(`{"posts":[`)
		rc, i := C.step_fill(stFeed, &row), 0
		for ; rc == sqliteRow; rc = C.step_fill(stFeed, &row) {
			if i > 0 {
				puts(",")
			}
			i++
			putRow(&row)
		}
		C.sqlite3_reset(stFeed)
		if rc != sqliteDone {
			dbFail()
			arena = arena[:mark]
			errResp(500, internalErr, close)
			return
		}
		puts("]}")
		finish()
		return
	}
	if len(path) > 7 && string(path[:7]) == "/posts/" {
		seg := path[7:]
		sl := bytes.IndexByte(seg, '/')
		if get && sl < 0 {
			id := parseID(seg)
			if id < 0 {
				errResp(400, "invalid post id", close)
				return
			}
			if !txn() {
				errResp(500, internalErr, close)
				return
			}
			rc := C.post_get(stPost, C.longlong(id), &row)
			switch rc {
			case sqliteRow:
				beginResp(200, close)
				puts(`{"post":`)
				putRow(&row)
				puts("}")
				finish()
			case sqliteDone:
				errResp(404, "post not found", close)
			default:
				dbFail()
				errResp(500, internalErr, close)
			}
			C.sqlite3_reset(stPost)
			return
		}
		if post && sl >= 0 && string(seg[sl:]) == "/like" {
			uid, _, e := auth(authh)
			if e != "" {
				errResp(401, e, close)
				return
			}
			id := parseID(seg[:sl])
			if id < 0 {
				errResp(400, "invalid post id", close)
				return
			}
			if !txn() {
				errResp(500, internalErr, close)
				return
			}
			var changes C.int
			rc := wstep(func() C.int { return C.like_step(db, stLike, C.longlong(uid), C.longlong(id), &changes) })
			if rc != sqliteDone {
				dbFail()
				errResp(500, internalErr, close)
				return
			}
			created := changes > 0
			if !created {
				rc = C.exists_step(stExists, C.longlong(id))
				if rc == sqliteDone {
					errResp(404, "post not found", close)
					return
				}
				if rc != sqliteRow {
					dbFail()
					errResp(500, internalErr, close)
					return
				}
			}
			if created {
				beginResp(201, close)
				puts(`{"liked":true,"already_liked":false,"post_id":`)
			} else {
				beginResp(200, close)
				puts(`{"liked":true,"already_liked":true,"post_id":`)
			}
			putu(id)
			puts("}")
			finish()
			return
		}
	}
	if post && string(path) == "/posts" {
		uid, uname, e := auth(authh)
		if e != "" {
			errResp(401, e, close)
			return
		}
		w := [1]want{{key: "body"}}
		if !jdoc(body, w[:]) {
			errResp(400, "malformed JSON body", close)
			return
		}
		if !w[0].found || body[w[0].v] != '"' {
			errResp(400, "body is required", close)
			return
		}
		_, n := jstr(body[:w[0].ve], w[0].v, txt[:])
		if n < 0 {
			errResp(400, "malformed JSON body", close)
			return
		}
		s, se := 0, n
		for s < se {
			l := jswsAt(txt[:se], s)
			if l == 0 {
				break
			}
			s += l
		}
		for se > s {
			l := jswsBefore(txt[:], s, se)
			if l == 0 {
				break
			}
			se -= l
		}
		if s == se {
			errResp(400, "body is required", close)
			return
		}
		chars := 0
		for _, c := range txt[s:se] {
			if c&0xc0 != 0x80 {
				chars++
			}
		}
		if chars > 500 {
			errResp(400, "body must be at most 500 characters", close)
			return
		}
		if !txn() {
			errResp(500, internalErr, close)
			return
		}
		var newID C.longlong
		var cn C.int
		text := txt[s:se]
		rc := wstep(func() C.int {
			return C.ins_step(stIns, C.longlong(uid), (*C.char)(unsafe.Pointer(&text[0])), C.int(len(text)), &newID, &cn)
		})
		if rc != sqliteRow {
			dbFail()
			errResp(500, internalErr, close)
			return
		}
		beginResp(201, close)
		puts(`{"post":{"id":`)
		putu(int64(newID))
		puts(`,"body":"`)
		putjs(text)
		puts(`","created_at":"`)
		put(unsafe.Slice((*byte)(unsafe.Pointer(&C.created_buf[0])), int(cn)))
		puts(`","author":"`)
		putjs(uname)
		puts(`","like_count":0}}`)
		finish()
		return
	}
	if get && string(path) == "/health" {
		if C.step_reset(stOne) == sqliteRow {
			beginResp(200, close)
			puts(`{"status":"ok","db":"ok","uptime_s":`)
			putu(nowS - started)
			puts("}")
		} else {
			beginResp(503, close)
			puts(`{"status":"degraded","db":"unreachable","error":"`)
			msg := C.sqlite3_errmsg(db)
			putjs(unsafe.Slice((*byte)(unsafe.Pointer(msg)), int(C.strlen(msg))))
			puts(`"}`)
		}
		finish()
		return
	}
	errResp(404, "not found", close)
}

func hdrIs(k []byte, name string) bool { // ASCII case-insensitive header name match
	if len(k) != len(name) {
		return false
	}
	for i := range k {
		c := k[i]
		if c >= 'A' && c <= 'Z' {
			c += 32
		}
		if c != name[i] {
			return false
		}
	}
	return true
}

func trimOWS(v []byte) []byte {
	for len(v) > 0 && (v[0] == ' ' || v[0] == '\t') {
		v = v[1:]
	}
	for len(v) > 0 && (v[len(v)-1] == ' ' || v[len(v)-1] == '\t') {
		v = v[:len(v)-1]
	}
	return v
}

var crlf2 = []byte("\r\n\r\n")

// serve handles every complete request in buf. Returns bytes consumed, or -1 to drop the connection.
func serve(fd int, buf []byte, closing *bool) int {
	used := 0
	for used < len(buf) && !*closing {
		p := buf[used:]
		if len(arena)+2*maxReq > batchOut {
			arenaFull = true // the rest waits for the next batch
			break
		}
		he := bytes.Index(p, crlf2)
		if he < 0 {
			if len(p) > maxReq {
				return -1
			}
			break
		}
		le := bytes.IndexByte(p[:he+2], '\r')
		line := p[:le]
		sp1 := bytes.IndexByte(line, ' ')
		if sp1 < 0 {
			return -1
		}
		sp2r := bytes.IndexByte(line[sp1+1:], ' ')
		if sp2r < 0 {
			return -1
		}
		sp2 := sp1 + 1 + sp2r
		close := string(line[sp2+1:]) == "HTTP/1.0"
		var authh []byte
		clen, expect := 0, false
		for h := le + 2; h < he+2; {
			hl := h + bytes.IndexByte(p[h:he+2], '\r')
			if c := bytes.IndexByte(p[h:hl], ':'); c >= 0 {
				k, v := p[h:h+c], trimOWS(p[h+c+1:hl])
				switch {
				case hdrIs(k, "content-length"):
					clen = 0
					for _, d := range v {
						if d < '0' || d > '9' {
							return -1
						}
						if clen = clen*10 + int(d-'0'); clen > maxReq {
							return -1
						}
					}
				case hdrIs(k, "authorization"):
					authh = v
				case hdrIs(k, "connection"):
					if hdrIs(v, "close") {
						close = true
					} else if hdrIs(v, "keep-alive") {
						close = false
					}
				case hdrIs(k, "expect"):
					expect = hdrIs(v, "100-continue")
				case hdrIs(k, "transfer-encoding"):
					return -1 // chunked uploads aren't supported
				}
			}
			h = hl + 2
		}
		body := he + 4
		if len(p)-body < clen {
			if expect && body == len(p) {
				const cont = "HTTP/1.1 100 Continue\r\n\r\n"
				if sysWrite(fd, []byte(cont)) < 0 {
					return -1
				}
			}
			break
		}
		handle(line[:sp1], line[sp1+1:sp2], authh, p[body:body+clen], close)
		if close {
			*closing = true
		}
		used += body + clen
	}
	return used
}

// ---------- raw syscalls (the loop never blocks except in epoll_wait) ----------
func sysRead(fd int, b []byte) int {
	n, _, e := syscall.RawSyscall(syscall.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	if e != 0 {
		if e == syscall.EAGAIN {
			return -2
		}
		return -1
	}
	return int(n)
}

func sysWrite(fd int, b []byte) int {
	n, _, e := syscall.RawSyscall(syscall.SYS_WRITE, uintptr(fd), uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	if e != 0 {
		if e == syscall.EAGAIN {
			return -2
		}
		return -1
	}
	return int(n)
}

func epollSet(op, fd int, ev uint32) {
	e := syscall.EpollEvent{Events: ev, Fd: int32(fd)}
	syscall.EpollCtl(epfd, op, fd, &e)
}

// ---------- connections ----------
func connClose(fd int) {
	conns[fd] = conn{}
	syscall.Close(fd)
	nconns--
}

func process(fd int, buf []byte, own bool) {
	c := &conns[fd]
	start := len(arena)
	closing := false
	arenaFull = false
	used := serve(fd, buf, &closing)
	if used < 0 {
		arena = arena[:start]
		connClose(fd)
		return
	}
	if len(arena) > start {
		segs = append(segs, seg{fd, start, len(arena) - start})
	}
	c.closing = closing
	left := len(buf) - used
	if closing {
		left = 0
	}
	if left > 0 {
		if !own {
			c.in = append(make([]byte, 0, left+rbufSize), buf[used:]...)
		} else if used > 0 {
			c.in = c.in[:copy(c.in, c.in[used:])]
		}
		if arenaFull {
			deferred = append(deferred, fd)
		}
	} else {
		c.in = nil
	}
	if closing && len(arena) == start {
		connClose(fd)
	}
}

func onReadable(fd int) {
	c := &conns[fd]
	if c.out != nil {
		return // still sending an earlier response; reading resumes once it is out
	}
	if len(c.in) > 0 {
		if cap(c.in)-len(c.in) < 4096 {
			c.in = append(make([]byte, 0, cap(c.in)*2+rbufSize), c.in...)
		}
		r := sysRead(fd, c.in[len(c.in):cap(c.in)])
		if r == -2 {
			return
		}
		if r <= 0 {
			connClose(fd)
			return
		}
		c.in = c.in[:len(c.in)+r]
		process(fd, c.in, true)
		return
	}
	r := sysRead(fd, rbuf[:])
	if r == -2 {
		return
	}
	if r <= 0 {
		connClose(fd)
		return
	}
	process(fd, rbuf[:r], false)
}

func onWritable(fd int) {
	c := &conns[fd]
	if c.out == nil {
		return
	}
	w := sysWrite(fd, c.out[c.outoff:])
	if w == -2 {
		return
	}
	if w < 0 {
		connClose(fd)
		return
	}
	c.outoff += w
	if c.outoff < len(c.out) {
		return
	}
	c.out, c.outoff = nil, 0
	if c.closing {
		connClose(fd)
		return
	}
	epollSet(syscall.EPOLL_CTL_MOD, fd, syscall.EPOLLIN)
	if len(c.in) > 0 {
		process(fd, c.in, true)
	}
}

// flushBatch commits the batch, then sends every response produced in it.
func flushBatch() {
	if inTxn {
		if C.step_reset(stCommit) != sqliteDone {
			C.step_reset(stRollback)
			if txnWrites {
				batchFailed = true
			}
		}
		inTxn, txnWrites = false, false
	}
	for _, s := range segs {
		c := &conns[s.fd]
		if !c.open {
			continue
		}
		if batchFailed { // the batch wasn't committed: never claim it was
			connClose(s.fd)
			continue
		}
		out := arena[s.off : s.off+s.n]
		w := sysWrite(s.fd, out)
		if w == -1 {
			connClose(s.fd)
			continue
		}
		if w == -2 {
			w = 0
		}
		if w < s.n {
			c.out = append([]byte(nil), out[w:]...)
			c.outoff = 0
			epollSet(syscall.EPOLL_CTL_MOD, s.fd, syscall.EPOLLOUT)
		} else if c.closing {
			connClose(s.fd)
		}
	}
	segs = segs[:0]
	arena = arena[:0]
	batchFailed = false
}

func prep(sql string) *C.sqlite3_stmt {
	cs := C.CString(sql)
	defer C.free(unsafe.Pointer(cs))
	var s *C.sqlite3_stmt
	if C.sqlite3_prepare_v3(db, cs, -1, C.SQLITE_PREPARE_PERSISTENT, &s, nil) != 0 {
		fatal("prepare %s: %s", sql, C.GoString(C.sqlite3_errmsg(db)))
	}
	return s
}

func execSQL(sql string) C.int {
	cs := C.CString(sql)
	defer C.free(unsafe.Pointer(cs))
	return C.sqlite3_exec(db, cs, nil, nil, nil)
}

func dbOpen(path string) {
	cp := C.CString(path)
	if C.sqlite3_open_v2(cp, &db, C.SQLITE_OPEN_READWRITE, nil) != 0 {
		fatal("open %s: %s", path, C.GoString(C.sqlite3_errmsg(db)))
	}
	C.sqlite3_busy_timeout(db, 5000)
	if execSQL("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000;"+
		"SELECT count(*) FROM users WHERE id = 1;") != 0 {
		fatal("pragma: %s", C.GoString(C.sqlite3_errmsg(db)))
	}
	// The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
	// for a long time), so the WAL stays bounded at about 256 MB.
	execSQL("PRAGMA wal_autocheckpoint=65536;")
	cols := "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "
	stFeed = prep(cols + "ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
	stPost = prep(cols + "WHERE p.id = ?")
	stIns = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
	stLike = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING")
	stExists = prep("SELECT 1 FROM posts WHERE id = ?")
	stOne = prep("SELECT 1")
	stBegin, stBeginImm, stCommit, stRollback = prep("BEGIN"), prep("BEGIN IMMEDIATE"), prep("COMMIT"), prep("ROLLBACK")
	C.step_fill(stFeed, &row) // takes the lock and touches the hot pages before the first request
	C.sqlite3_reset(stFeed)
}

// ---------- listener and elastic workers ----------
var (
	listenAddr              syscall.SockaddrInet4
	lfd                     = -1
	readyFD                 = -1
	connHi, connLo, connMax int
	spawnPending, spawned   bool
)

func listenStart() {
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		fatal("socket: %v", err)
	}
	syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, soReusePort, 1)
	syscall.SetsockoptInt(fd, syscall.IPPROTO_TCP, syscall.TCP_NODELAY, 1)
	if err := syscall.Bind(fd, &listenAddr); err != nil {
		fatal("bind: %v", err)
	}
	if err := syscall.Listen(fd, 65535); err != nil {
		fatal("listen: %v", err)
	}
	epollSet(syscall.EPOLL_CTL_ADD, fd, syscall.EPOLLIN)
	lfd = fd
}

func acceptConns(limit int) {
	for nconns < limit {
		c, _, e := syscall.RawSyscall6(syscall.SYS_ACCEPT4, uintptr(lfd), 0, 0, syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0, 0)
		if e != 0 {
			break
		}
		fd := int(c)
		if fd >= maxFD {
			syscall.Close(fd)
			continue
		}
		conns[fd].open = true
		nconns++
		epollSet(syscall.EPOLL_CTL_ADD, fd, syscall.EPOLLIN)
	}
}

func listenStop() { // take what is already queued, then leave new clients to the other worker
	acceptConns(connMax)
	syscall.Close(lfd)
	lfd = -1
}

// Every keep-alive client holds a descriptor, and LimitNOFILE caps descriptors per process. Close to that
// cap this process starts one more copy of itself on the same port (SO_REUSEPORT) and stops accepting
// once the copy is listening, so new clients go to the copy. Below the cap there is one process.
func spawnWorker() {
	spawnPending, spawned = false, true
	var p [2]int
	if err := syscall.Pipe2(p[:], syscall.O_CLOEXEC); err != nil {
		fmt.Fprintln(os.Stderr, "pipe:", err)
		return
	}
	pid, err := syscall.ForkExec("/proc/self/exe", []string{"server", "--shared", "3"}, &syscall.ProcAttr{
		Env:   os.Environ(),
		Files: []uintptr{0, 1, 2, uintptr(p[1])},
		Sys:   &syscall.SysProcAttr{Pdeathsig: syscall.SIGTERM},
	})
	syscall.Close(p[1])
	if err != nil { // keep accepting here
		syscall.Close(p[0])
		spawned = false
		epollSet(syscall.EPOLL_CTL_ADD, lfd, syscall.EPOLLIN)
		fmt.Fprintln(os.Stderr, "spawn:", err)
		return
	}
	readyFD = p[0]
	epollSet(syscall.EPOLL_CTL_ADD, readyFD, syscall.EPOLLIN)
	fmt.Fprintf(os.Stderr, "%d connections: starting worker %d\n", nconns, pid)
}

// Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
// the length of the fsync, so a helper process does them and the event loop never waits on the disk.
func checkpointer(path string) {
	if C.sqlite3_open_v2(C.CString(path), &db, C.SQLITE_OPEN_READWRITE, nil) != 0 {
		os.Exit(1)
	}
	C.sqlite3_busy_timeout(db, 100)
	execSQL("PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;")
	for {
		var log, done C.int
		C.sqlite3_wal_checkpoint_v2(db, nil, C.SQLITE_CHECKPOINT_PASSIVE, &log, &done)
		if log > 0 && done < log {
			time.Sleep(time.Millisecond)
		} else {
			time.Sleep(10 * time.Millisecond)
		}
	}
}

func run(notifyFD int) {
	var err error
	if epfd, err = syscall.EpollCreate1(syscall.EPOLL_CLOEXEC); err != nil {
		fatal("epoll: %v", err)
	}
	listenStart()
	if notifyFD >= 0 { // tell the parent worker we're listening
		syscall.Write(notifyFD, []byte{1})
		syscall.Close(notifyFD)
	}
	evs := make([]syscall.EpollEvent, 512)
	todo := make([]int, 0, 64)
	for {
		n, err := syscall.EpollWait(epfd, evs, -1)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			fatal("epoll_wait: %v", err)
		}
		nowS = time.Now().Unix()
		for i := 0; i < n; i++ {
			fd := int(evs[i].Fd)
			switch {
			case fd == lfd:
				acceptConns(connHi)
				if nconns >= connHi { // nearly out of descriptors: new clients go to another worker
					if spawned {
						listenStop()
					} else {
						syscall.EpollCtl(epfd, syscall.EPOLL_CTL_DEL, lfd, nil)
						spawnPending = true
					}
				}
			case fd == readyFD: // the new worker is listening
				var b [1]byte
				up := sysRead(readyFD, b[:]) == 1
				syscall.Close(readyFD)
				readyFD = -1
				if up {
					listenStop()
				} else { // it failed to start
					spawned = false
					epollSet(syscall.EPOLL_CTL_ADD, lfd, syscall.EPOLLIN)
				}
			case conns[fd].open:
				if e := evs[i].Events; e&syscall.EPOLLOUT != 0 {
					onWritable(fd)
				} else if e&(syscall.EPOLLIN|syscall.EPOLLHUP|syscall.EPOLLERR) != 0 {
					onReadable(fd)
				}
			}
		}
		flushBatch()
		for len(deferred) > 0 { // requests held back by a full arena
			todo = append(todo[:0], deferred...)
			deferred = deferred[:0]
			for _, fd := range todo {
				if c := &conns[fd]; c.open && len(c.in) > 0 && c.out == nil {
					process(fd, c.in, true)
				}
			}
			flushBatch()
		}
		if spawnPending {
			spawnWorker()
		}
		if lfd < 0 && nconns < connLo {
			listenStart()
		}
	}
}

func fatal(f string, a ...any) {
	fmt.Fprintf(os.Stderr, f+"\n", a...)
	os.Exit(1)
}

func main() {
	runtime.GOMAXPROCS(1)
	runtime.LockOSThread()
	debug.SetGCPercent(400) // the hot path barely allocates; collect rarely
	debug.SetMemoryLimit(1 << 30)
	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		fatal("SQLITE_PATH and JWT_SECRET are required")
	}
	host, port := os.Getenv("HOST"), os.Getenv("PORT")
	if host == "" {
		host = "0.0.0.0"
	}
	if port == "" {
		port = "3000"
	}
	ip := net.ParseIP(host).To4()
	pn, err := strconv.Atoi(port)
	if ip == nil || err != nil {
		fatal("bad HOST/PORT %s:%s", host, port)
	}
	listenAddr.Port = pn
	copy(listenAddr.Addr[:], ip)
	C.sqlite3_initialize() // built with SQLITE_OMIT_AUTOINIT
	if len(os.Args) > 1 && os.Args[1] == "--ckpt" {
		checkpointer(path)
	}
	notifyFD := -1
	if len(os.Args) > 2 { // --shared <fd>
		notifyFD, _ = strconv.Atoi(os.Args[2])
	}
	signal.Ignore(syscall.SIGPIPE, syscall.SIGCHLD) // children are reaped automatically
	var rl syscall.Rlimit                           // Go already raised the soft limit to the hard one
	syscall.Getrlimit(syscall.RLIMIT_NOFILE, &rl)
	lim := int(min(rl.Cur, maxFD))
	connHi = lim - max(lim/16, 64) // leave room for SQLite's files and the listener
	connLo = connHi - connHi/8
	connMax = lim - 16
	mac = hmac.New(sha256.New, []byte(secret)) // keyed once; Reset() restores the keyed state per token
	started = time.Now().Unix()
	nowS = started
	dbOpen(path)
	if len(os.Args) == 1 { // the first process starts the checkpointer; workers share it
		syscall.ForkExec("/proc/self/exe", []string{"server", "--ckpt"}, &syscall.ProcAttr{
			Env: os.Environ(), Files: []uintptr{0, 1, 2}, Sys: &syscall.SysProcAttr{Pdeathsig: syscall.SIGTERM}})
	}
	run(notifyFD)
}
