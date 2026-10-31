// A minimal feed API: net/http + database/sql + mattn/go-sqlite3.
package main

import (
	"bufio"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	_ "net/http/pprof"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	_ "github.com/mattn/go-sqlite3"
)

type post struct {
	ID        int64
	Body      string
	CreatedAt string
	Author    string
	LikeCount int64
}

type writeKind int

const (
	writeCreate writeKind = iota
	writeLike
)

type writeJob struct {
	kind     writeKind
	userID   int64
	username string
	body     string
	postID   int64
	reply    chan writeResult
}

type writeResult struct {
	post     post
	already  bool
	notFound bool
	failed   bool
}

type app struct {
	readDB  *sql.DB
	writeDB *sql.DB
	secret  []byte
	started time.Time

	feed    *sql.Stmt
	getPost *sql.Stmt

	insertPost *sql.Stmt
	postExists *sql.Stmt
	insertLike *sql.Stmt

	writeCh     chan *writeJob
	batchWindow time.Duration
}

// Reused by every response; the buffer is only returned to the pool after the
// response body has been written to the connection.
var bufPool = sync.Pool{
	New: func() any {
		b := make([]byte, 0, 4096)
		return &b
	},
}

func main() {
	host := os.Getenv("HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "3000"
	}

	dsn := "file:" + os.Getenv("SQLITE_PATH") +
		"?_busy_timeout=5000&_journal_mode=WAL&_synchronous=NORMAL" +
		"&_cache_size=-16384&_mmap_size=268435456"

	readDB, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	readDB.SetMaxOpenConns(4)
	readDB.SetMaxIdleConns(4)

	writeDB, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	writeDB.SetMaxOpenConns(1)
	writeDB.SetMaxIdleConns(1)

	batchMS := 100
	if v := os.Getenv("WRITE_BATCH_MS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			batchMS = n
		}
	}

	a := &app{
		readDB:      readDB,
		writeDB:     writeDB,
		secret:      []byte(os.Getenv("JWT_SECRET")),
		started:     time.Now(),
		writeCh:     make(chan *writeJob, 256),
		batchWindow: time.Duration(batchMS) * time.Millisecond,
	}
	a.feed, err = readDB.Prepare(`
		SELECT p.id, p.body, p.created_at, u.username,
		       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
		  FROM posts p JOIN users u ON u.id = p.user_id
		 ORDER BY p.created_at DESC, p.id DESC
		 LIMIT 20`)
	if err != nil {
		log.Fatal(err)
	}
	a.getPost, err = readDB.Prepare(`
		SELECT p.id, p.body, p.created_at, u.username,
		       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
		  FROM posts p JOIN users u ON u.id = p.user_id
		 WHERE p.id = ?`)
	if err != nil {
		log.Fatal(err)
	}
	a.insertPost, err = writeDB.Prepare(
		"INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
	if err != nil {
		log.Fatal(err)
	}
	a.postExists, err = writeDB.Prepare("SELECT 1 FROM posts WHERE id = ?")
	if err != nil {
		log.Fatal(err)
	}
	a.insertLike, err = writeDB.Prepare(
		"INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
	if err != nil {
		log.Fatal(err)
	}

	go a.writeLoop()

	// Opt-in profiling listener; unused in the benchmark runs.
	if addr := os.Getenv("PPROF_ADDR"); addr != "" {
		go func() {
			log.Println("pprof listening on", addr)
			log.Println(http.ListenAndServe(addr, nil))
		}()
	}

	ln, err := net.Listen("tcp", host+":"+port)
	if err != nil {
		log.Fatal(err)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			log.Print(err)
			continue
		}
		go a.serveConn(c)
	}
}

// serveConn is net/http's connection loop with an 8 KiB write buffer. The
// stock server hardcodes 4 KiB (newBufioWriterSize(..., 4<<10)), which forces
// the 5 KiB feed response into two write syscalls.
func (a *app) serveConn(c net.Conn) {
	defer c.Close()
	if tc, ok := c.(*net.TCPConn); ok {
		tc.SetNoDelay(true)
	}
	br := bufio.NewReaderSize(c, 4096)
	bw := bufio.NewWriterSize(c, 8192)
	for {
		req, err := http.ReadRequest(br)
		if err != nil {
			return
		}
		resp := &capture{status: http.StatusOK}
		a.ServeHTTP(resp, req)
		if req.Body != nil {
			req.Body.Close()
		}
		bw.WriteString("HTTP/1.1 ")
		bw.WriteString(strconv.Itoa(resp.status))
		bw.WriteByte(' ')
		bw.WriteString(http.StatusText(resp.status))
		bw.WriteString("\r\nContent-Type: application/json\r\nContent-Length: ")
		bw.WriteString(strconv.Itoa(len(resp.body)))
		if req.Close {
			bw.WriteString("\r\nConnection: close")
		}
		bw.WriteString("\r\n\r\n")
		if _, err := bw.Write(resp.body); err != nil {
			return
		}
		if err := bw.Flush(); err != nil {
			return
		}
		if req.Close {
			return
		}
	}
}

type capture struct {
	status int
	body   []byte
	hdr    http.Header
}

func (c *capture) Header() http.Header {
	if c.hdr == nil {
		c.hdr = make(http.Header)
	}
	return c.hdr
}

func (c *capture) WriteHeader(status int) { c.status = status }

func (c *capture) Write(p []byte) (int, error) {
	c.body = append(c.body, p...)
	return len(p), nil
}

// writeLoop batches create/like jobs into one transaction. Every response waits
// for the commit, so a 201/200 is still only sent for durable rows.
func (a *app) writeLoop() {
	for first := range a.writeCh {
		jobs := make([]*writeJob, 1, 64)
		jobs[0] = first

		if a.batchWindow > 0 {
			timer := time.NewTimer(a.batchWindow)
		collect:
			for len(jobs) < cap(jobs) {
				select {
				case j := <-a.writeCh:
					jobs = append(jobs, j)
				case <-timer.C:
					break collect
				}
			}
			timer.Stop()
		} else {
		drain:
			for len(jobs) < cap(jobs) {
				select {
				case j := <-a.writeCh:
					jobs = append(jobs, j)
				default:
					break drain
				}
			}
		}
		a.runBatch(jobs)
	}
}

func (a *app) runBatch(jobs []*writeJob) {
	tx, err := a.writeDB.Begin()
	if err != nil {
		failJobs(jobs)
		return
	}
	insertPost := tx.Stmt(a.insertPost)
	postExists := tx.Stmt(a.postExists)
	insertLike := tx.Stmt(a.insertLike)

	results := make([]writeResult, len(jobs))
	ok := true
run:
	for i, j := range jobs {
		switch j.kind {
		case writeCreate:
			var p post
			p.Body, p.Author = j.body, j.username
			if err := insertPost.QueryRow(j.userID, j.body).Scan(&p.ID, &p.CreatedAt); err != nil {
				ok = false
				break run
			}
			results[i].post = p
		case writeLike:
			var one int
			err := postExists.QueryRow(j.postID).Scan(&one)
			if err == sql.ErrNoRows {
				results[i].notFound = true
				continue
			}
			if err != nil {
				ok = false
				break run
			}
			res, err := insertLike.Exec(j.userID, j.postID)
			if err != nil {
				ok = false
				break run
			}
			n, err := res.RowsAffected()
			if err != nil {
				ok = false
				break run
			}
			results[i].already = n == 0
		}
	}

	if ok {
		err = tx.Commit()
	} else {
		tx.Rollback()
	}
	if !ok || err != nil {
		failJobs(jobs)
		return
	}
	for i, j := range jobs {
		j.reply <- results[i]
	}
}

func failJobs(jobs []*writeJob) {
	for _, j := range jobs {
		j.reply <- writeResult{failed: true}
	}
}

func (a *app) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch p := r.URL.Path; p {
	case "/health":
		a.health(w, r)
	case "/feed":
		a.feedPosts(w, r)
	case "/posts":
		a.createPost(w, r)
	default:
		const prefix = "/posts/"
		if !strings.HasPrefix(p, prefix) {
			writeError(w, http.StatusNotFound, "not found")
			return
		}
		rest := p[len(prefix):]
		if id, ok := strings.CutSuffix(rest, "/like"); ok {
			a.likePost(w, r, id)
			return
		}
		a.getPostByID(w, r, rest)
	}
}

func (a *app) health(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	var one int
	bp := bufPool.Get().(*[]byte)
	b := (*bp)[:0]
	if err := a.readDB.QueryRowContext(ctx, "SELECT 1").Scan(&one); err != nil {
		b = append(b, `{"status":"degraded","db":"unreachable","error":`...)
		b = appendString(b, err.Error())
		b = append(b, '}')
		*bp = b
		writeBody(w, http.StatusServiceUnavailable, b)
		bufPool.Put(bp)
		return
	}
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started).Seconds()), 10)
	b = append(b, '}')
	*bp = b
	writeBody(w, http.StatusOK, b)
	bufPool.Put(bp)
}

func (a *app) feedPosts(w http.ResponseWriter, r *http.Request) {
	rows, err := a.feed.QueryContext(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	defer rows.Close()

	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"posts":[`...)
	first := true
	for rows.Next() {
		var p post
		if err := rows.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount); err != nil {
			bufPool.Put(bp)
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		if !first {
			b = append(b, ',')
		}
		first = false
		b = appendPost(b, p)
	}
	if err := rows.Err(); err != nil {
		bufPool.Put(bp)
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	b = append(b, `]}`...)
	*bp = b
	writeBody(w, http.StatusOK, b)
	bufPool.Put(bp)
}

func (a *app) getPostByID(w http.ResponseWriter, r *http.Request, idStr string) {
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	var p post
	err := a.getPost.QueryRowContext(r.Context(), id).
		Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount)
	if err == sql.ErrNoRows {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"post":`...)
	b = appendPost(b, p)
	b = append(b, '}')
	*bp = b
	writeBody(w, http.StatusOK, b)
	bufPool.Put(bp)
}

func (a *app) createPost(w http.ResponseWriter, r *http.Request) {
	userID, username, authErr := a.authenticate(r)
	if authErr != "" {
		writeError(w, http.StatusUnauthorized, authErr)
		return
	}

	data, err := io.ReadAll(r.Body)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	var payload struct {
		Body json.RawMessage `json:"body"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		writeError(w, http.StatusBadRequest, "malformed JSON body")
		return
	}
	var body string
	if payload.Body == nil || json.Unmarshal(payload.Body, &body) != nil {
		writeError(w, http.StatusBadRequest, "body is required")
		return
	}
	body = strings.TrimSpace(body)
	if body == "" {
		writeError(w, http.StatusBadRequest, "body is required")
		return
	}
	if utf8.RuneCountInString(body) > 500 {
		writeError(w, http.StatusBadRequest, "body must be at most 500 characters")
		return
	}

	job := &writeJob{
		kind:     writeCreate,
		userID:   userID,
		username: username,
		body:     body,
		reply:    make(chan writeResult, 1),
	}
	a.writeCh <- job
	result := <-job.reply
	if result.failed {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"post":`...)
	b = appendPost(b, result.post)
	b = append(b, '}')
	*bp = b
	writeBody(w, http.StatusCreated, b)
	bufPool.Put(bp)
}

func (a *app) likePost(w http.ResponseWriter, r *http.Request, idStr string) {
	userID, _, authErr := a.authenticate(r)
	if authErr != "" {
		writeError(w, http.StatusUnauthorized, authErr)
		return
	}
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}

	job := &writeJob{
		kind:   writeLike,
		userID: userID,
		postID: id,
		reply:  make(chan writeResult, 1),
	}
	a.writeCh <- job
	result := <-job.reply
	if result.failed {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	if result.notFound {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	status := http.StatusCreated
	if result.already {
		status = http.StatusOK
	}
	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"liked":true,"already_liked":`...)
	b = strconv.AppendBool(b, result.already)
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, '}')
	*bp = b
	writeBody(w, status, b)
	bufPool.Put(bp)
}

// authenticate verifies the bearer JWT and returns the user id and username.
// A non-empty third result is the error message to send with status 401.
func (a *app) authenticate(r *http.Request) (int64, string, string) {
	const (
		missing = "missing bearer token"
		invalid = "invalid or expired token"
		payload = "invalid token payload"
	)
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, "Bearer ") {
		return 0, "", missing
	}
	parts := strings.Split(header[len("Bearer "):], ".")
	if len(parts) != 3 {
		return 0, "", invalid
	}

	head, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return 0, "", invalid
	}
	var hdr struct {
		Alg string `json:"alg"`
	}
	if json.Unmarshal(head, &hdr) != nil || hdr.Alg != "HS256" {
		return 0, "", invalid
	}

	sig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return 0, "", invalid
	}
	mac := hmac.New(sha256.New, a.secret)
	mac.Write([]byte(parts[0]))
	mac.Write([]byte("."))
	mac.Write([]byte(parts[1]))
	if !hmac.Equal(sig, mac.Sum(nil)) {
		return 0, "", invalid
	}

	claimsJSON, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return 0, "", invalid
	}
	var claims struct {
		Sub      json.RawMessage `json:"sub"`
		Username json.RawMessage `json:"username"`
		Exp      int64           `json:"exp"`
	}
	if json.Unmarshal(claimsJSON, &claims) != nil || claims.Exp <= time.Now().Unix() {
		return 0, "", invalid
	}

	var sub string
	if json.Unmarshal(claims.Sub, &sub) != nil {
		return 0, "", payload
	}
	userID, ok := parsePositiveInt(sub)
	if !ok {
		return 0, "", payload
	}
	var username string
	if json.Unmarshal(claims.Username, &username) != nil {
		return 0, "", payload
	}
	return userID, username, ""
}

func parsePostID(s string) (int64, bool) {
	return parsePositiveInt(s)
}

// parsePositiveInt accepts only a positive decimal integer string.
func parsePositiveInt(s string) (int64, bool) {
	if s == "" {
		return 0, false
	}
	for i := range s {
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

func appendPost(dst []byte, p post) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, p.ID, 10)
	dst = append(dst, `,"body":`...)
	dst = appendString(dst, p.Body)
	dst = append(dst, `,"created_at":`...)
	dst = appendString(dst, p.CreatedAt)
	dst = append(dst, `,"author":`...)
	dst = appendString(dst, p.Author)
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, p.LikeCount, 10)
	return append(dst, '}')
}

// appendString writes a JSON string. Non-ASCII is emitted as UTF-8 (the spec
// makes escaping optional); quotes, backslashes and control bytes are escaped.
func appendString(dst []byte, s string) []byte {
	dst = append(dst, '"')
	start := 0
	for i := range len(s) {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		dst = append(dst, s[start:i]...)
		switch c {
		case '"':
			dst = append(dst, '\\', '"')
		case '\\':
			dst = append(dst, '\\', '\\')
		case '\n':
			dst = append(dst, '\\', 'n')
		case '\r':
			dst = append(dst, '\\', 'r')
		case '\t':
			dst = append(dst, '\\', 't')
		default:
			const hex = "0123456789abcdef"
			dst = append(dst, '\\', 'u', '0', '0', hex[c>>4], hex[c&0x0f])
		}
		start = i + 1
	}
	dst = append(dst, s[start:]...)
	return append(dst, '"')
}

func writeError(w http.ResponseWriter, status int, msg string) {
	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"error":`...)
	b = appendString(b, msg)
	b = append(b, '}')
	*bp = b
	writeBody(w, status, b)
	bufPool.Put(bp)
}

func writeBody(w http.ResponseWriter, status int, body []byte) {
	w.WriteHeader(status)
	w.Write(body)
}
