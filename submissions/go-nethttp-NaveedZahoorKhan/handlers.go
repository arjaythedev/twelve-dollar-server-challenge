package main

import (
	"database/sql/driver"
	"io"
	"net/http"
	"strconv"
	"strings"

	sqlite3 "github.com/mattn/go-sqlite3"
)

// postRow holds one projected post row.
func asInt(v driver.Value) int64 {
	switch n := v.(type) {
	case int64:
		return n
	case float64:
		return int64(n)
	}
	return 0
}

func asStr(v driver.Value) string {
	switch s := v.(type) {
	case string:
		return s
	case []byte:
		return string(s)
	}
	return ""
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

func router(w http.ResponseWriter, r *http.Request) {
	p := r.URL.Path
	switch {
	case p == "/health" && r.Method == http.MethodGet:
		handleHealth(w)
	case p == "/feed" && r.Method == http.MethodGet:
		handleFeed(w)
	case p == "/posts" && r.Method == http.MethodPost:
		handleCreatePost(w, r)
	case strings.HasPrefix(p, "/posts/"):
		rest := p[len("/posts/"):]
		if strings.HasSuffix(rest, "/like") {
			idStr := rest[:len(rest)-len("/like")]
			if !strings.Contains(idStr, "/") && r.Method == http.MethodPost {
				handleLike(w, r, idStr)
				return
			}
			writeError(w, 404, "not found")
			return
		}
		if !strings.Contains(rest, "/") && r.Method == http.MethodGet {
			handleGetPost(w, rest)
			return
		}
		writeError(w, 404, "not found")
	default:
		writeError(w, 404, "not found")
	}
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

func handleHealth(w http.ResponseWriter) {
	healthM.Lock()
	rows, err := health.Query(nil)
	if err == nil {
		dest := make([]driver.Value, 1)
		err = rows.Next(dest)
		if err == io.EOF {
			err = nil
		}
		rows.Close()
	}
	healthM.Unlock()
	if err != nil {
		b := []byte(`{"status":"degraded","db":"unreachable","error":`)
		b = appendJSONString(b, err.Error())
		b = append(b, '}')
		writeJSON(w, 503, b)
		return
	}
	up := int64(timeSinceStartSeconds())
	b := []byte(`{"status":"ok","db":"ok","uptime_s":`)
	b = strconv.AppendInt(b, up, 10)
	b = append(b, '}')
	writeJSON(w, 200, b)
}

func handleFeed(w http.ResponseWriter) {
	bp := getBuf()
	defer putBuf(bp)
	out, err := buildFeedJSON((*bp)[:0])
	if err != nil {
		writeError(w, 500, "internal server error")
		return
	}
	*bp = out // keep cap for return to pool
	writeJSON(w, 200, out)
}

// buildFeedJSON runs the feed query on a pooled reader and appends the full
// response body into buf (which the caller owns, typically from the pool).
// Uses the batched single-cgo-call row reader (StepPost5) to avoid per-column
// cgo crossings and driver.Value boxing on the hot path.
func buildFeedJSON(buf []byte) ([]byte, error) {
	rd := <-readers
	if err := rd.feed.ResetStmt(); err != nil {
		readers <- rd
		return nil, err
	}
	buf = append(buf, `{"posts":[`...)
	first := true
	var err error
	for {
		row, ok, e := rd.feed.StepPost5()
		if e != nil {
			err = e
			break
		}
		if !ok {
			break
		}
		if !first {
			buf = append(buf, ',')
		}
		first = false
		buf = appendPost(buf, row.ID, row.Body, row.CreatedAt, row.Author, row.LikeCount)
	}
	// Reset so the statement is clean for the next borrower (also releases locks).
	_ = rd.feed.ResetStmt()
	readers <- rd
	if err != nil {
		return nil, err
	}
	buf = append(buf, ']', '}')
	return buf, nil
}

func handleGetPost(w http.ResponseWriter, idStr string) {
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, 400, "invalid post id")
		return
	}
	rd := <-readers
	var (
		found bool
		row   sqlite3.PostRow
		err   error
	)
	if e := rd.byID.BindInt1Reset(id); e != nil {
		err = e
	} else {
		row, found, err = rd.byID.StepPost5()
		_ = rd.byID.ResetStmt()
	}
	readers <- rd
	if err != nil {
		writeError(w, 500, "internal server error")
		return
	}
	if !found {
		writeError(w, 404, "post not found")
		return
	}
	bp := getBuf()
	defer putBuf(bp)
	buf := append((*bp)[:0], `{"post":`...)
	buf = appendPost(buf, row.ID, row.Body, row.CreatedAt, row.Author, row.LikeCount)
	buf = append(buf, '}')
	*bp = buf
	writeJSON(w, 200, buf)
}

func handleCreatePost(w http.ResponseWriter, r *http.Request) {
	// Auth first.
	uid, username, status, msg := parseAuth(r.Header.Get("Authorization"))
	if status != 0 {
		writeError(w, status, msg)
		return
	}
	// Read body (<=16KB tested; cap generously).
	raw, _ := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	body, perr := parseBodyField(raw)
	switch perr {
	case errMalformed:
		writeError(w, 400, "malformed JSON body")
		return
	case errBodyRequired:
		writeError(w, 400, "body is required")
		return
	case errBodyTooLong:
		writeError(w, 400, "body must be at most 500 characters")
		return
	}

	rq := &writeReq{kind: kindPost, uid: uid, body: body}
	wr.submit(rq)
	if rq.err != nil {
		writeError(w, 500, "internal server error")
		return
	}
	buf := make([]byte, 0, 256+len(body))
	buf = append(buf, `{"post":`...)
	buf = appendPost(buf, rq.newID, body, rq.createdAt, username, 0)
	buf = append(buf, '}')
	writeJSON(w, 201, buf)
}

func handleLike(w http.ResponseWriter, r *http.Request, idStr string) {
	// Auth first, then id (per SPEC).
	uid, _, status, msg := parseAuth(r.Header.Get("Authorization"))
	if status != 0 {
		writeError(w, status, msg)
		return
	}
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, 400, "invalid post id")
		return
	}

	rq := &writeReq{kind: kindLike, uid: uid, postID: id}
	wr.submit(rq)
	if rq.err != nil {
		writeError(w, 500, "internal server error")
		return
	}
	if rq.notFound {
		writeError(w, 404, "post not found")
		return
	}
	buf := make([]byte, 0, 64)
	buf = append(buf, `{"liked":true,"already_liked":`...)
	if rq.alreadyLiked {
		buf = append(buf, "true"...)
	} else {
		buf = append(buf, "false"...)
	}
	buf = append(buf, `,"post_id":`...)
	buf = strconv.AppendInt(buf, id, 10)
	buf = append(buf, '}')
	status = 201
	if rq.alreadyLiked {
		status = 200
	}
	writeJSON(w, status, buf)
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

// parsePostID accepts only a positive base-10 integer with no sign, decimal,
// or leading zeros issues. "0", "-1", "abc", "1.5" are all invalid.
func parsePostID(s string) (int64, bool) {
	if s == "" {
		return 0, false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n <= 0 {
		return 0, false
	}
	return n, true
}
