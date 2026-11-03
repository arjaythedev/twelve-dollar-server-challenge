package main

import (
	"bytes"
	"log"
	"strconv"
	"time"
	"unicode/utf8"
)

const maxBodyChars = 500

var (
	errNotFound     = []byte(`{"error":"not found"}`)
	errInvalidID    = []byte(`{"error":"invalid post id"}`)
	errPostNotFound = []byte(`{"error":"post not found"}`)
	errMissingAuth  = []byte(`{"error":"missing bearer token"}`)
	errInvalidToken = []byte(`{"error":"invalid or expired token"}`)
	errTokenPayload = []byte(`{"error":"invalid token payload"}`)
	errMalformed    = []byte(`{"error":"malformed JSON body"}`)
	errBodyRequired = []byte(`{"error":"body is required"}`)
	errBodyTooLong  = []byte(`{"error":"body must be at most 500 characters"}`)
	errInternal     = []byte(`{"error":"internal server error"}`)

	pathFeed   = []byte("/feed")
	pathHealth = []byte("/health")
	pathPosts  = []byte("/posts")
	prefPosts  = []byte("/posts/")
	suffLike   = []byte("/like")
)

type app struct {
	db      *db
	jwt     *verifier
	started time.Time
	body    []byte
	scratch []byte

	writes, spare []pendingWrite
	arena, arena2 []byte
	wout          []byte
	marks         []int32
}

type writeKind uint8

const (
	writeCreate writeKind = iota
	writeLike
)

// pendingWrite is a validated, authenticated write waiting for the batch's commit. Its text and
// username live in app.arena.
type pendingWrite struct {
	fd                int32
	kind              writeKind
	keepAlive, http10 bool
	uid, postID       int64
	textOff, textEnd  int32
	userOff, userEnd  int32
}

func newApp(d *db, secret string) *app {
	return &app{
		db:      d,
		jwt:     newVerifier(secret),
		started: time.Now(),
		body:    make([]byte, 0, outCap+1024),
		scratch: make([]byte, 0, 256),
		writes:  make([]pendingWrite, 0, maxEvents),
		spare:   make([]pendingWrite, 0, maxEvents),
		arena:   make([]byte, 0, 256<<10),
		arena2:  make([]byte, 0, 256<<10),
		wout:    make([]byte, 0, 64<<10),
		marks:   make([]int32, 0, maxEvents+1),
	}
}

type reply struct {
	s      *server
	r      *request
	ok     bool
	queued bool
}

// serve answers r into the server's write buffer, or queues it and reports true when it writes.
func (a *app) serve(s *server, r *request, fd int) bool {
	w := reply{s: s, r: r}
	defer func() {
		if p := recover(); p != nil {
			log.Printf("panic serving %s %s: %v", r.method, r.path, p)
			if !w.ok {
				w.send(500, errInternal)
			}
		}
	}()
	a.route(&w, fd)
	return w.queued
}

func (w *reply) send(status int, body []byte) {
	w.ok = true
	w.s.respond(status, body, w.r.keepAlive, w.r.http10)
}

func (a *app) route(w *reply, fd int) {
	r := w.r
	get, post := string(r.method) == "GET", string(r.method) == "POST"
	switch {
	case get && bytes.Equal(r.path, pathFeed):
		a.feed(w)
	case get && bytes.Equal(r.path, pathHealth):
		a.health(w)
	case post && bytes.Equal(r.path, pathPosts):
		a.create(w, fd)
	case bytes.HasPrefix(r.path, prefPosts):
		rest := r.path[len(prefPosts):]
		switch {
		case get && bytes.IndexByte(rest, '/') < 0:
			id, ok := parsePositiveID(rest)
			if !ok {
				w.send(400, errInvalidID)
				return
			}
			a.post(w, id)
		case post && bytes.HasSuffix(rest, suffLike) && bytes.IndexByte(rest[:len(rest)-len(suffLike)], '/') < 0:
			a.like(w, fd, rest[:len(rest)-len(suffLike)])
		default:
			w.send(404, errNotFound)
		}
	default:
		w.send(404, errNotFound)
	}
}

func (a *app) health(w *reply) {
	b := a.body[:0]
	if _, err := a.db.run(a.db.ping, nil); err != nil {
		b = append(b, `{"status":"degraded","db":"unreachable","error":"`...)
		b = appendEscaped(b, []byte(err.Error()))
		w.send(503, append(b, `"}`...))
		return
	}
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started)/time.Second), 10)
	w.send(200, append(b, '}'))
}

func (a *app) feed(w *reply) {
	rows, err := a.db.run(a.db.feed, nil)
	if err != nil {
		a.fail(w, "feed", err)
		return
	}
	rr := rowReader{b: a.db.out[:a.db.used()]}
	b := append(a.body[:0], `{"posts":[`...)
	for i := 0; i < rows; i++ {
		if i > 0 {
			b = append(b, ',')
		}
		b = appendPostRow(b, &rr)
	}
	w.send(200, append(b, "]}"...))
}

func (a *app) post(w *reply, id int64) {
	rows, err := a.db.run(a.db.post, nil, id)
	if err != nil {
		a.fail(w, "post", err)
		return
	}
	if rows == 0 {
		w.send(404, errPostNotFound)
		return
	}
	rr := rowReader{b: a.db.out[:a.db.used()]}
	b := append(a.body[:0], `{"post":`...)
	b = appendPostRow(b, &rr)
	w.send(200, append(b, '}'))
}

// appendPostRow renders one packed (id, body, created_at, username, like_count) row.
func appendPostRow(b []byte, rr *rowReader) []byte {
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, rr.int(), 10)
	b = append(b, `,"body":"`...)
	b = appendEscaped(b, rr.text())
	b = append(b, `","created_at":"`...)
	b = appendEscaped(b, rr.text())
	b = append(b, `","author":"`...)
	b = appendEscaped(b, rr.text())
	b = append(b, `","like_count":`...)
	b = strconv.AppendInt(b, rr.int(), 10)
	return append(b, '}')
}

func (a *app) authenticate(w *reply) (int64, []byte, bool) {
	uid, user, res := a.jwt.verify(w.r.auth, w.r.hasAuth)
	switch res {
	case authMissing:
		w.send(401, errMissingAuth)
	case authInvalid:
		w.send(401, errInvalidToken)
	case authPayload:
		w.send(401, errTokenPayload)
	}
	return uid, user, res == authOK
}

func (a *app) create(w *reply, fd int) {
	uid, user, ok := a.authenticate(w)
	if !ok {
		return
	}
	in := w.r.body
	if !validJSON(in) {
		w.send(400, errMalformed)
		return
	}
	var text []byte
	if obj, isObj := newObjectWalker(in); isObj {
		for m, more := obj.next(); more; m, more = obj.next() {
			if keyIs(m.key, "body", a.scratch) {
				text = nil
				if m.val[0] == '"' {
					text = unescape(a.db.text[:0], m.val)
				}
			}
		}
	}
	text = trimSpace(text)
	if len(text) == 0 {
		w.send(400, errBodyRequired)
		return
	}
	if utf8.RuneCount(text) > maxBodyChars {
		w.send(400, errBodyTooLong)
		return
	}
	pw := pendingWrite{fd: int32(fd), kind: writeCreate, uid: uid}
	pw.textOff = int32(len(a.arena))
	a.arena = append(a.arena, text...)
	pw.textEnd, pw.userOff = int32(len(a.arena)), int32(len(a.arena))
	a.arena = append(a.arena, user...)
	pw.userEnd = int32(len(a.arena))
	a.queue(w, pw)
}

func (a *app) like(w *reply, fd int, rawID []byte) {
	uid, _, ok := a.authenticate(w)
	if !ok {
		return
	}
	id, ok := parsePositiveID(rawID)
	if !ok {
		w.send(400, errInvalidID)
		return
	}
	a.queue(w, pendingWrite{fd: int32(fd), kind: writeLike, uid: uid, postID: id})
}

func (a *app) fail(w *reply, op string, err error) {
	log.Printf("%s: %v", op, err)
	w.send(500, errInternal)
}

func (a *app) queue(w *reply, pw pendingWrite) {
	pw.keepAlive, pw.http10 = w.r.keepAlive, w.r.http10
	a.writes = append(a.writes, pw)
	w.ok, w.queued = true, true
}

// commitWrites runs every write queued in this batch inside one transaction (group commit) and
// sends each response only after COMMIT returns. The write lock is held for the inserts alone,
// never while sockets are being served, so the other worker process waits microseconds for it.
// A connection that pipelined more requests behind its write is resumed afterwards and may
// queue again; the caller loops until nothing is queued. It returns false, leaving the queue as
// it is, when the write lock is held elsewhere: the loop retries within a millisecond while it
// keeps serving reads.
func (a *app) commitWrites(s *server) bool {
	ok, err := a.db.tryBegin()
	if err != nil {
		log.Printf("begin: %v", err)
	} else if !ok {
		return false
	}
	batch, text := a.writes, a.arena
	a.writes, a.spare = a.spare[:0], batch
	a.arena, a.arena2 = a.arena2[:0], text

	out := a.wout[:0]
	marks := a.marks[:0]
	for i := range batch {
		pw := &batch[i]
		start := len(out)
		if ok {
			out = a.execWrite(out, s.date, pw, text)
		}
		marks = append(marks, int32(start))
	}
	if ok {
		if _, err := a.db.run(a.db.commit, nil); err != nil {
			log.Printf("commit: %v", err)
			if _, rerr := a.db.run(a.db.rollback, nil); rerr != nil {
				log.Printf("rollback: %v", rerr)
			}
			ok = false
		}
	}
	if !ok {
		out, marks = out[:0], marks[:0]
		for i := range batch {
			marks = append(marks, int32(len(out)))
			out = appendResponse(out, s.date, 500, errInternal, batch[i].keepAlive, batch[i].http10)
		}
	}
	marks = append(marks, int32(len(out)))
	a.wout, a.marks = out, marks
	for i := range batch {
		s.deliverWrite(int(batch[i].fd), out[marks[i]:marks[i+1]])
	}
	return true
}

// execWrite runs one queued write inside the open transaction and appends its full response.
func (a *app) execWrite(out, date []byte, pw *pendingWrite, text []byte) []byte {
	b := a.body[:0]
	switch pw.kind {
	case writeCreate:
		body := text[pw.textOff:pw.textEnd]
		rows, err := a.db.run(a.db.create, body, pw.uid)
		if err != nil || rows != 1 {
			log.Printf("create: %v", err)
			return appendResponse(out, date, 500, errInternal, pw.keepAlive, pw.http10)
		}
		rr := rowReader{b: a.db.out[:a.db.used()]}
		b = append(b, `{"post":{"id":`...)
		b = strconv.AppendInt(b, rr.int(), 10)
		b = append(b, `,"body":"`...)
		b = appendEscaped(b, body)
		b = append(b, `","created_at":"`...)
		b = appendEscaped(b, rr.text())
		b = append(b, `","author":"`...)
		b = appendEscaped(b, text[pw.userOff:pw.userEnd])
		b = append(b, `","like_count":0}}`...)
		return appendResponse(out, date, 201, b, pw.keepAlive, pw.http10)
	default:
		if _, err := a.db.run(a.db.like, nil, pw.uid, pw.postID); err != nil {
			log.Printf("like: %v", err)
			return appendResponse(out, date, 500, errInternal, pw.keepAlive, pw.http10)
		}
		status, already := 201, false
		if a.db.changes() == 0 {
			rows, err := a.db.run(a.db.exists, nil, pw.postID)
			if err != nil {
				log.Printf("like exists: %v", err)
				return appendResponse(out, date, 500, errInternal, pw.keepAlive, pw.http10)
			}
			if rows == 0 {
				return appendResponse(out, date, 404, errPostNotFound, pw.keepAlive, pw.http10)
			}
			status, already = 200, true
		}
		b = append(b, `{"liked":true,"already_liked":`...)
		b = strconv.AppendBool(b, already)
		b = append(b, `,"post_id":`...)
		b = strconv.AppendInt(b, pw.postID, 10)
		return appendResponse(out, date, status, append(b, '}'), pw.keepAlive, pw.http10)
	}
}
