// A conventional Go service: net/http + database/sql + a SQLite driver.
// The HTTP server, JSON, JWT and batching are all standard-library Go.
package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net/http"
	_ "net/http/pprof"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
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

type readResp struct {
	status int
	body   []byte
}

type readCall struct {
	done chan struct{}
	resp readResp
}

type readGroup struct {
	mu      sync.Mutex
	m       map[string]*readCall
	enabled bool
}

func (g *readGroup) do(key string, window time.Duration, fn func() readResp) readResp {
	if !g.enabled {
		return fn()
	}
	g.mu.Lock()
	if c, ok := g.m[key]; ok {
		g.mu.Unlock()
		<-c.done
		return c.resp
	}
	c := &readCall{done: make(chan struct{})}
	if g.m == nil {
		g.m = make(map[string]*readCall)
	}
	g.m[key] = c
	g.mu.Unlock()

	if window > 0 {
		time.Sleep(window)
	}
	c.resp = fn()

	g.mu.Lock()
	delete(g.m, key)
	g.mu.Unlock()
	close(c.done)
	return c.resp
}

type app struct {
	secret  []byte
	started time.Time

	readDB  *sql.DB
	feed    *sql.Stmt
	getPost *sql.Stmt
	health  *sql.Stmt

	writeDB    *sql.DB
	writeConn  *sql.Conn
	insertPost *sql.Stmt
	postExists *sql.Stmt
	insertLike *sql.Stmt

	writeCh     chan *writeJob
	batchWindow time.Duration

	readsGroup readGroup
	readGen    atomic.Uint64
	readWindow time.Duration
}

func main() {
	host := envOr("HOST", "127.0.0.1")
	port := envOr("PORT", "3000")
	path := os.Getenv("SQLITE_PATH")

	batchMS := 100
	if v := os.Getenv("WRITE_BATCH_MS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			batchMS = n
		}
	}
	windowUS := 500
	if v := os.Getenv("READ_BATCH_US"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			windowUS = n
		}
	}
	readConns := 2
	if v := os.Getenv("READ_CONNS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			readConns = n
		}
	}
	coalesce := false
	if v := os.Getenv("READ_COALESCE"); v != "" {
		if b, err := strconv.ParseBool(v); err == nil {
			coalesce = b
		}
	}

	dsn := path + "?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000" +
		"&_cache_size=-16384&_mmap_size=268435456&_mutex=no"

	readDB, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	readDB.SetMaxOpenConns(readConns)
	readDB.SetMaxIdleConns(readConns)
	if err := readDB.Ping(); err != nil {
		log.Fatal(err)
	}

	writeDB, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	writeDB.SetMaxOpenConns(1)
	writeDB.SetMaxIdleConns(1)
	if err := writeDB.Ping(); err != nil {
		log.Fatal(err)
	}
	wconn, err := writeDB.Conn(context.Background())
	if err != nil {
		log.Fatal(err)
	}

	a := &app{
		secret:      []byte(os.Getenv("JWT_SECRET")),
		started:     time.Now(),
		readDB:      readDB,
		writeDB:     writeDB,
		writeConn:   wconn,
		writeCh:     make(chan *writeJob, 256),
		batchWindow: time.Duration(batchMS) * time.Millisecond,
		readWindow:  time.Duration(windowUS) * time.Microsecond,
		readsGroup:  readGroup{enabled: coalesce},
	}
	if a.feed, err = readDB.Prepare(feedSQL); err != nil {
		log.Fatal(err)
	}
	if a.getPost, err = readDB.Prepare(getPostSQL); err != nil {
		log.Fatal(err)
	}
	if a.health, err = readDB.Prepare(healthSQL); err != nil {
		log.Fatal(err)
	}
	ctx := context.Background()
	if a.insertPost, err = wconn.PrepareContext(ctx, insertPostSQL); err != nil {
		log.Fatal(err)
	}
	if a.postExists, err = wconn.PrepareContext(ctx, postExistsSQL); err != nil {
		log.Fatal(err)
	}
	if a.insertLike, err = wconn.PrepareContext(ctx, insertLikeSQL); err != nil {
		log.Fatal(err)
	}
	go a.writeLoop()

	if addr := os.Getenv("PPROF_ADDR"); addr != "" {
		go func() {
			log.Println("pprof listening on", addr)
			log.Println(http.ListenAndServe(addr, nil))
		}()
	}

	srv := &http.Server{
		Addr:              host + ":" + port,
		Handler:           a,
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       75 * time.Second,
	}
	log.Fatal(srv.ListenAndServe())
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

const (
	feedSQL = `
		SELECT json_object('posts', json_group_array(json_object(
			'id', p.id,
			'body', p.body,
			'created_at', p.created_at,
			'author', p.username,
			'like_count', p.like_count)))
		FROM (
			SELECT p.id, p.body, p.created_at, u.username,
			       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
			  FROM posts p JOIN users u ON u.id = p.user_id
			 ORDER BY p.created_at DESC, p.id DESC
			 LIMIT 20
		) p`
	getPostSQL = `
		SELECT json_object('post', json_object(
			'id', p.id,
			'body', p.body,
			'created_at', p.created_at,
			'author', u.username,
			'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)))
		  FROM posts p JOIN users u ON u.id = p.user_id
		 WHERE p.id = ?`
	insertPostSQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
	postExistsSQL = "SELECT 1 FROM posts WHERE id = ?"
	insertLikeSQL = "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING"
	healthSQL     = "SELECT 1"
)

func (a *app) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch p := r.URL.Path; p {
	case "/health":
		a.healthHandler(w, r)
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

func (a *app) healthHandler(w http.ResponseWriter, r *http.Request) {
	var one int
	err := a.health.QueryRow().Scan(&one)
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, "database unavailable")
		return
	}
	b := make([]byte, 0, 64)
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started).Seconds()), 10)
	b = append(b, '}')
	writeJSON(w, http.StatusOK, b)
}

func (a *app) feedPosts(w http.ResponseWriter, r *http.Request) {
	key := "feed:" + strconv.FormatUint(a.readGen.Load(), 10)
	resp := a.readsGroup.do(key, a.readWindow, a.loadFeed)
	writeJSON(w, resp.status, resp.body)
}

func (a *app) loadFeed() readResp {
	var body []byte
	if err := a.feed.QueryRow().Scan(&body); err != nil {
		return readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
	}
	return readResp{status: http.StatusOK, body: body}
}

func (a *app) getPostByID(w http.ResponseWriter, r *http.Request, idStr string) {
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	key := "post:" + strconv.FormatUint(a.readGen.Load(), 10) + ":" + strconv.FormatInt(id, 10)
	resp := a.readsGroup.do(key, a.readWindow, func() readResp {
		var body []byte
		err := a.getPost.QueryRow(id).Scan(&body)
		if err == sql.ErrNoRows {
			return readResp{status: http.StatusNotFound, body: []byte(`{"error":"post not found"}`)}
		}
		if err != nil {
			return readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
		}
		return readResp{status: http.StatusOK, body: body}
	})
	writeJSON(w, resp.status, resp.body)
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
		kind: writeCreate, userID: userID, username: username, body: body,
		reply: make(chan writeResult, 1),
	}
	a.writeCh <- job
	result := <-job.reply
	if result.failed {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	b := make([]byte, 0, 256)
	b = append(b, `{"post":`...)
	b = appendPost(b, result.post)
	b = append(b, '}')
	writeJSON(w, http.StatusCreated, b)
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
		kind: writeLike, userID: userID, postID: id,
		reply: make(chan writeResult, 1),
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
	b := make([]byte, 0, 64)
	b = append(b, `{"liked":true,"already_liked":`...)
	b = strconv.AppendBool(b, result.already)
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, '}')
	writeJSON(w, status, b)
}

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
	ctx := context.Background()
	if _, err := a.writeConn.ExecContext(ctx, "BEGIN IMMEDIATE"); err != nil {
		failJobs(jobs)
		return
	}
	results := make([]writeResult, len(jobs))
	ok := true
run:
	for i, j := range jobs {
		switch j.kind {
		case writeCreate:
			var id int64
			var created string
			err := a.insertPost.QueryRowContext(ctx, j.userID, j.body).Scan(&id, &created)
			if err != nil {
				ok = false
				break run
			}
			results[i].post = post{
				ID: id, Body: j.body, CreatedAt: created, Author: j.username,
			}
		case writeLike:
			var one int
			err := a.postExists.QueryRowContext(ctx, j.postID).Scan(&one)
			if err == sql.ErrNoRows {
				results[i].notFound = true
				continue
			}
			if err != nil {
				ok = false
				break run
			}
			res, err := a.insertLike.ExecContext(ctx, j.userID, j.postID)
			if err != nil {
				ok = false
				break run
			}
			n, _ := res.RowsAffected()
			results[i].already = n == 0
		}
	}
	if ok {
		_, okErr := a.writeConn.ExecContext(ctx, "COMMIT")
		ok = okErr == nil
	}
	if !ok {
		a.writeConn.ExecContext(ctx, "ROLLBACK")
		failJobs(jobs)
		return
	}
	a.readGen.Add(1)
	for i, j := range jobs {
		j.reply <- results[i]
	}
}

func failJobs(jobs []*writeJob) {
	for _, j := range jobs {
		j.reply <- writeResult{failed: true}
	}
}

func parsePostID(s string) (int64, bool) {
	return parsePositiveInt(s)
}

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
	b := make([]byte, 0, 64)
	b = append(b, `{"error":`...)
	b = appendString(b, msg)
	b = append(b, '}')
	writeJSON(w, status, b)
}

var jsonContentType = []string{"application/json"}

func writeJSON(w http.ResponseWriter, status int, body []byte) {
	h := w.Header()
	h["Content-Type"] = jsonContentType
	h["Content-Length"] = []string{strconv.Itoa(len(body))}
	w.WriteHeader(status)
	_, _ = w.Write(body)
}
