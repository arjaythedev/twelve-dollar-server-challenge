package main

import (
	"bytes"
	"log"
	"time"

	"feedapi/internal/sqlite"
)

const (
	errNotFound       = "not found"
	errInvalidPostID  = "invalid post id"
	errPostNotFound   = "post not found"
	errMissingBearer  = "missing bearer token"
	errInvalidToken   = "invalid or expired token"
	errInvalidPayload = "invalid token payload"
	errInternal       = "internal server error"
	errLengthRequired = "length required"
)

var (
	pathFeed       = []byte("/feed")
	pathHealth     = []byte("/health")
	pathPosts      = []byte("/posts")
	pathPostsSlash = []byte("/posts/")
	suffixLike     = []byte("/like")
)

// handle appends one response for req to out. held reports that the response
// confirms a write in the open batch transaction and must wait for commitBatch.
func (s *server) handle(req *request, out []byte, closeConn bool, now int64) ([]byte, bool) {
	s.scratch = s.scratch[:0]
	if req.chunked {
		return appendResponse(out, 411, appendError(s.scratch, errLengthRequired), true), false
	}
	switch req.method {
	case methodGET:
		switch {
		case bytes.Equal(req.path, pathFeed):
			return s.feed(out, closeConn)
		case bytes.Equal(req.path, pathHealth):
			return s.health(out, closeConn, now)
		case bytes.HasPrefix(req.path, pathPostsSlash):
			seg := req.path[len(pathPostsSlash):]
			if len(seg) > 0 && bytes.IndexByte(seg, '/') < 0 {
				return s.getPost(seg, out, closeConn)
			}
		}
	case methodPOST:
		switch {
		case bytes.Equal(req.path, pathPosts):
			return s.createPost(req, out, closeConn, now)
		case bytes.HasPrefix(req.path, pathPostsSlash) && bytes.HasSuffix(req.path, suffixLike):
			seg := req.path[len(pathPostsSlash) : len(req.path)-len(suffixLike)]
			if len(seg) > 0 && bytes.IndexByte(seg, '/') < 0 {
				return s.likePost(req, seg, out, closeConn, now)
			}
		}
	}
	return s.errorResponse(out, 404, errNotFound, closeConn), false
}

func (s *server) errorResponse(out []byte, status int, msg string, closeConn bool) []byte {
	return appendResponse(out, status, appendError(s.scratch[:0], msg), closeConn)
}

func (s *server) internalError(out []byte, closeConn bool, err error) []byte {
	log.Printf("internal error: %v", err)
	return s.errorResponse(out, 500, errInternal, closeConn)
}

func (s *server) health(out []byte, closeConn bool, now int64) ([]byte, bool) {
	st := s.stmts.health
	_, err := st.Step()
	st.Reset()
	if err != nil {
		b := append(s.scratch, `{"status":"degraded","db":"unreachable","error":`...)
		b = appendJSONString(b, []byte(err.Error()))
		b = append(b, '}')
		return appendResponse(out, 503, b, closeConn), false
	}
	uptime := int64(time.Since(s.start).Seconds())
	return appendResponse(out, 200, appendHealthOK(s.scratch, uptime), closeConn), false
}

func (s *server) feed(out []byte, closeConn bool) ([]byte, bool) {
	st := s.stmts.feed
	b := append(s.scratch, `{"posts":[`...)
	first := true
	for {
		row, err := st.Step()
		if err != nil {
			return s.internalError(out, closeConn, err), false
		}
		if !row {
			break
		}
		if !first {
			b = append(b, ',')
		}
		first = false
		b = appendPost(b, st.ColumnInt64(0), st.ColumnRawText(1), st.ColumnRawText(2), st.ColumnRawText(3), st.ColumnInt64(4))
	}
	st.Reset()
	b = append(b, "]}"...)
	s.scratch = b[:0] // keep the grown capacity for the next response
	return appendResponse(out, 200, b, closeConn), false
}

func (s *server) getPost(seg []byte, out []byte, closeConn bool) ([]byte, bool) {
	id, ok := parsePostID(seg)
	if !ok {
		return s.errorResponse(out, 400, errInvalidPostID, closeConn), false
	}
	st := s.stmts.post
	st.BindInt64(1, id)
	row, err := st.Step()
	if err != nil {
		return s.internalError(out, closeConn, err), false
	}
	if !row {
		st.Reset()
		return s.errorResponse(out, 404, errPostNotFound, closeConn), false
	}
	b := append(s.scratch, `{"post":`...)
	b = appendPost(b, st.ColumnInt64(0), st.ColumnRawText(1), st.ColumnRawText(2), st.ColumnRawText(3), st.ColumnInt64(4))
	b = append(b, '}')
	st.Reset()
	s.scratch = b[:0]
	return appendResponse(out, 200, b, closeConn), false
}

func (s *server) requireAuth(req *request, out []byte, closeConn bool, now int64) (claims, []byte, bool) {
	c, st := s.auth.authenticate(req.auth, now)
	switch st {
	case authOK:
		return c, nil, true
	case authMissing:
		return c, s.errorResponse(out, 401, errMissingBearer, closeConn), false
	case authBadPayload:
		return c, s.errorResponse(out, 401, errInvalidPayload, closeConn), false
	default:
		return c, s.errorResponse(out, 401, errInvalidToken, closeConn), false
	}
}

func (s *server) createPost(req *request, out []byte, closeConn bool, now int64) ([]byte, bool) {
	c, res, ok := s.requireAuth(req, out, closeConn, now)
	if !ok {
		return res, false
	}
	body, msg := validateBody(req.body)
	if msg != "" {
		return s.errorResponse(out, 400, msg, closeConn), false
	}
	if err := s.beginTx(); err != nil {
		return s.internalError(out, closeConn, err), false
	}
	st := s.stmts.create
	st.BindInt64(1, c.userID)
	st.BindText(2, body)
	row, err := st.Step()
	if err != nil || !row {
		st.Reset()
		if err == nil {
			err = &sqlite.Error{Msg: "insert returned no row"}
		}
		return s.internalError(out, closeConn, err), false
	}
	b := append(s.scratch, `{"post":`...)
	b = appendPost(b, st.ColumnInt64(0), []byte(body), st.ColumnRawText(1), []byte(c.username), 0)
	b = append(b, '}')
	st.Reset()
	return appendResponse(out, 201, b, closeConn), true
}

func (s *server) likePost(req *request, seg []byte, out []byte, closeConn bool, now int64) ([]byte, bool) {
	c, res, ok := s.requireAuth(req, out, closeConn, now)
	if !ok {
		return res, false
	}
	id, ok := parsePostID(seg)
	if !ok {
		return s.errorResponse(out, 400, errInvalidPostID, closeConn), false
	}
	if err := s.beginTx(); err != nil {
		return s.internalError(out, closeConn, err), false
	}
	st := s.stmts.like
	st.BindInt64(1, c.userID)
	st.BindInt64(2, id)
	_, err := st.Step()
	st.Reset()
	if err != nil {
		if sqlite.Code(err) == sqlite.ErrConstraintForeignKey {
			return s.errorResponse(out, 404, errPostNotFound, closeConn), false
		}
		return s.internalError(out, closeConn, err), false
	}
	inserted := s.db.Changes() == 1
	status := 200
	if inserted {
		status = 201
	}
	return appendResponse(out, status, appendLike(s.scratch, !inserted, id), closeConn), inserted
}
