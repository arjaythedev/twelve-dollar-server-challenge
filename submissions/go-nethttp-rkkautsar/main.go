// A conventional Go service: net/http + database/sql + a SQLite driver.
// The HTTP server, JSON, JWT and batching are all standard-library Go.
package main

import (
	"container/list"
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
	"runtime/debug"
	"sort"
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

// connTracker bounds resident keep-alive sockets. Above soft it asks clients
// to close after the current response (Connection: close), which is safe for
// non-idempotent requests. Above hard it closes the oldest idle socket as a
// last-resort valve. Closing an idle socket races with a client that sends at
// that instant, so soft is the primary mechanism.
type connTracker struct {
	mu    sync.Mutex
	idle  list.List
	elems map[net.Conn]*list.Element
	n     int
	soft  int
	hard  int
}

func newConnTracker(soft, hard int) *connTracker {
	return &connTracker{elems: make(map[net.Conn]*list.Element), soft: soft, hard: hard}
}

func (t *connTracker) state(c net.Conn, s http.ConnState) {
	var close []net.Conn
	t.mu.Lock()
	switch s {
	case http.StateNew:
		t.n++
	case http.StateActive:
		if e, ok := t.elems[c]; ok {
			t.idle.Remove(e)
			delete(t.elems, c)
		}
	case http.StateIdle:
		if _, ok := t.elems[c]; !ok {
			t.elems[c] = t.idle.PushBack(c)
		}
		for t.n > t.hard && t.idle.Len() > 0 {
			front := t.idle.Front()
			v := front.Value.(net.Conn)
			t.idle.Remove(front)
			delete(t.elems, v)
			close = append(close, v)
		}
	case http.StateClosed, http.StateHijacked:
		t.n--
		if e, ok := t.elems[c]; ok {
			t.idle.Remove(e)
			delete(t.elems, c)
		}
	}
	t.mu.Unlock()
	for _, c := range close {
		c.Close()
	}
}

func (t *connTracker) overSoft() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.n > t.soft
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

type postReadReq struct {
	id    int64
	gen   uint64
	reply chan readResp
}

type app struct {
	secret  []byte
	started time.Time

	readDB  *sql.DB
	feed    *sql.Stmt
	getPost *sql.Stmt
	health  *sql.Stmt

	writeDB         *sql.DB
	writeConn       *sql.Conn
	insertPostsBulk *sql.Stmt
	insertLikesBulk *sql.Stmt
	postsExistBulk  *sql.Stmt

	writeCh     chan *writeJob
	batchWindow time.Duration

	conns *connTracker

	readsGroup readGroup
	readGen    atomic.Uint64
	readWindow time.Duration
	postCh     chan postReadReq
	postBatch  bool
	postStmts  map[int]*sql.Stmt
}

func main() {
	// A soft Go heap target keeps GC ahead of the 2 GiB cgroup on the
	// official box. Respect an explicit GOMEMLIMIT, including the one the
	// Go runtime reads from the environment at startup.
	if os.Getenv("GOMEMLIMIT") == "" {
		debug.SetMemoryLimit(1200 << 20)
	}

	host := envOr("HOST", "127.0.0.1")
	port := envOr("PORT", "3000")
	path := os.Getenv("SQLITE_PATH")

	batchMS := 10
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
	readConns := 4
	if v := os.Getenv("READ_CONNS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			readConns = n
		}
	}
	coalesce := true
	if v := os.Getenv("READ_COALESCE"); v != "" {
		if b, err := strconv.ParseBool(v); err == nil {
			coalesce = b
		}
	}
	postBatch := true
	if v := os.Getenv("POST_BATCH"); v != "" {
		if b, err := strconv.ParseBool(v); err == nil {
			postBatch = b
		}
	}
	idleMS := 65000
	if v := os.Getenv("IDLE_TIMEOUT_MS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			idleMS = n
		}
	}
	maxConns := 46000
	if v := os.Getenv("MAX_CONNS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			maxConns = n
		}
	}
	softConns := 44000
	if v := os.Getenv("SOFT_CONNS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			softConns = n
		}
	}
	if softConns >= maxConns {
		softConns = maxConns - 2000
		if softConns < 1 {
			softConns = maxConns
		}
	}

	sqliteCacheKiB := 16384
	if v := os.Getenv("SQLITE_CACHE"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			sqliteCacheKiB = n
		}
	}
	// Bytes. The default disables mmap so the 256 MiB mapping does not
	// compete with the connection set on a 2 GiB box.
	sqliteMmapBytes := int64(0)
	if v := os.Getenv("SQLITE_MMAP"); v != "" {
		if n, err := strconv.ParseInt(v, 10, 64); err == nil && n >= 0 {
			sqliteMmapBytes = n
		}
	}

	dsn := path + "?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000" +
		"&_cache_size=-" + strconv.Itoa(sqliteCacheKiB) +
		"&_mmap_size=" + strconv.FormatInt(sqliteMmapBytes, 10) +
		"&_mutex=no&_wal_autocheckpoint=10000"

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
		postCh:      make(chan postReadReq, 1024),
		postBatch:   postBatch,
		postStmts:   make(map[int]*sql.Stmt),
		conns:       newConnTracker(softConns, maxConns),
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
	if a.insertPostsBulk, err = wconn.PrepareContext(ctx, insertPostsBulkSQL); err != nil {
		log.Fatal(err)
	}
	if a.insertLikesBulk, err = wconn.PrepareContext(ctx, insertLikesBulkSQL); err != nil {
		log.Fatal(err)
	}
	if a.postsExistBulk, err = wconn.PrepareContext(ctx, postsExistBulkSQL); err != nil {
		log.Fatal(err)
	}
	go a.writeLoop()
	if postBatch {
		go a.postBatchLoop()
	}

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
		IdleTimeout:       time.Duration(idleMS) * time.Millisecond,
		ConnState:         a.conns.state,
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
	insertPostsBulkSQL = `INSERT INTO posts (user_id, body)
		SELECT CAST(json_extract(value, '$.u') AS INTEGER), json_extract(value, '$.b')
		  FROM json_each(?1)
		RETURNING id, created_at`
	insertLikesBulkSQL = `INSERT INTO likes (user_id, post_id)
		SELECT CAST(json_extract(value, '$.u') AS INTEGER),
		       CAST(json_extract(value, '$.p') AS INTEGER)
		  FROM json_each(?1) WHERE true
		ON CONFLICT (user_id, post_id) DO NOTHING
		RETURNING user_id, post_id`
	postsExistBulkSQL = `SELECT id FROM posts
		 WHERE id IN (SELECT CAST(value AS INTEGER) FROM json_each(?1))`
	healthSQL = "SELECT 1"
)

func (a *app) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if a.conns.overSoft() && r.Method != http.MethodPost {
		// Close after reads, not writes: a client may chain a write on the
		// same connection (the LG sends like -> create immediately), and a
		// close here would turn that chained write into an EOF. GETs are the
		// majority, so the cap still drains.
		w.Header().Set("Connection", "close")
	}
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
	if !a.postBatch {
		resp := a.loadPost(id)
		writeJSON(w, resp.status, resp.body)
		return
	}
	gen := a.readGen.Load()
	reply := make(chan readResp, 1)
	a.postCh <- postReadReq{id: id, gen: gen, reply: reply}
	resp := <-reply
	// A write may have committed after this request was queued. Never
	// return a read that started before that commit.
	if a.readGen.Load() != gen {
		resp = a.loadPost(id)
	}
	writeJSON(w, resp.status, resp.body)
}

func (a *app) loadPost(id int64) readResp {
	var body []byte
	err := a.getPost.QueryRow(id).Scan(&body)
	if err == sql.ErrNoRows {
		return readResp{status: http.StatusNotFound, body: []byte(`{"error":"post not found"}`)}
	}
	if err != nil {
		return readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
	}
	return readResp{status: http.StatusOK, body: body}
}

const maxPostBatch = 256

// postBatchLoop serves concurrent GET /posts/:id requests with one IN query.
// Requests carry the write generation so a post-write request cannot accept a
// result produced before the commit.
func (a *app) postBatchLoop() {
	for first := range a.postCh {
		reqs := make([]postReadReq, 1, maxPostBatch)
		reqs[0] = first
		if len(a.postCh) > 0 {
			if a.readWindow > 0 {
				timer := time.NewTimer(a.readWindow)
			collect:
				for len(reqs) < maxPostBatch {
					select {
					case r := <-a.postCh:
						reqs = append(reqs, r)
					case <-timer.C:
						break collect
					}
				}
				timer.Stop()
			} else {
			drain:
				for len(reqs) < maxPostBatch {
					select {
					case r := <-a.postCh:
						reqs = append(reqs, r)
					default:
						break drain
					}
				}
			}
		}
		a.resolvePostBatch(reqs)
	}
}

func (a *app) resolvePostBatch(reqs []postReadReq) {
	ids := make([]int64, 0, len(reqs))
	seen := make(map[int64]struct{}, len(reqs))
	for _, req := range reqs {
		if _, ok := seen[req.id]; ok {
			continue
		}
		seen[req.id] = struct{}{}
		ids = append(ids, req.id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	n := len(ids)
	st, ok := a.postStmts[n]
	if !ok {
		ph := strings.TrimSuffix(strings.Repeat("?,", n), ",")
		q := strings.Replace(getPostSQL, "SELECT json_object", "SELECT p.id, json_object", 1)
		q = strings.Replace(q, "WHERE p.id = ?", "WHERE p.id IN ("+ph+")", 1)
		var err error
		st, err = a.readDB.Prepare(q)
		if err != nil {
			a.failPostBatch(reqs)
			return
		}
		a.postStmts[n] = st
	}
	args := make([]any, n)
	for i, id := range ids {
		args[i] = id
	}
	rows, err := st.Query(args...)
	if err != nil {
		a.failPostBatch(reqs)
		return
	}
	found := make(map[int64]readResp, len(ids))
	for rows.Next() {
		var id int64
		var body []byte
		if err := rows.Scan(&id, &body); err != nil {
			rows.Close()
			a.failPostBatch(reqs)
			return
		}
		found[id] = readResp{status: http.StatusOK, body: body}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		a.failPostBatch(reqs)
		return
	}
	rows.Close()
	for _, req := range reqs {
		resp, ok := found[req.id]
		if !ok {
			resp = readResp{status: http.StatusNotFound, body: []byte(`{"error":"post not found"}`)}
		}
		req.reply <- resp
	}
}

func (a *app) failPostBatch(reqs []postReadReq) {
	for _, req := range reqs {
		req.reply <- readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
	}
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
		jobs := make([]*writeJob, 1, 128)
		jobs[0] = first
		// Only wait for a batch when there is already a real backlog;
		// a nearly empty queue is committed immediately.
		if len(a.writeCh) >= 4 && a.batchWindow > 0 {
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

	// Creates: one multi-row INSERT ... json_each ... RETURNING. ids are
	// assigned in insertion order, so sorting them pairs each row with the
	// create job that produced it.
	type createIn struct {
		U int64  `json:"u"`
		B string `json:"b"`
	}
	createIdx := make([]int, 0, len(jobs))
	creates := make([]createIn, 0, len(jobs))
	for i, j := range jobs {
		if j.kind == writeCreate {
			createIdx = append(createIdx, i)
			creates = append(creates, createIn{U: j.userID, B: j.body})
		}
	}
	if len(creates) > 0 {
		buf, err := json.Marshal(creates)
		if err != nil {
			ok = false
		} else {
			rows, err := a.insertPostsBulk.QueryContext(ctx, string(buf))
			if err != nil {
				ok = false
			} else {
				type createRow struct {
					id      int64
					created string
				}
				got := make([]createRow, 0, len(creates))
				for rows.Next() {
					var id int64
					var created string
					if err := rows.Scan(&id, &created); err != nil {
						ok = false
						break
					}
					got = append(got, createRow{id, created})
				}
				if err := rows.Err(); err != nil {
					ok = false
				}
				rows.Close()
				if ok && len(got) != len(creates) {
					ok = false
				}
				if ok {
					sort.Slice(got, func(i, j int) bool { return got[i].id < got[j].id })
					for k, idx := range createIdx {
						j := jobs[idx]
						results[idx].post = post{
							ID: got[k].id, Body: j.body,
							CreatedAt: got[k].created, Author: j.username,
						}
					}
				}
			}
		}
	}

	// Likes: resolve post existence for the whole batch with one IN query,
	// then insert every new (user, post) pair with one multi-row statement.
	// RETURNING tells us which rows were new; conflicts are already-liked.
	if ok {
		type likeIn struct {
			U int64 `json:"u"`
			P int64 `json:"p"`
		}
		likeIdx := make([]int, 0, len(jobs))
		uniquePosts := make([]int64, 0, len(jobs))
		seenPost := make(map[int64]bool, len(jobs))
		for i, j := range jobs {
			if j.kind != writeLike {
				continue
			}
			likeIdx = append(likeIdx, i)
			if !seenPost[j.postID] {
				seenPost[j.postID] = true
				uniquePosts = append(uniquePosts, j.postID)
			}
		}
		if len(likeIdx) > 0 {
			exists := make(map[int64]bool, len(uniquePosts))
			idsBuf, err := json.Marshal(uniquePosts)
			if err != nil {
				ok = false
			} else {
				rows, err := a.postsExistBulk.QueryContext(ctx, string(idsBuf))
				if err != nil {
					ok = false
				} else {
					for rows.Next() {
						var id int64
						if err := rows.Scan(&id); err != nil {
							ok = false
							break
						}
						exists[id] = true
					}
					if err := rows.Err(); err != nil {
						ok = false
					}
					rows.Close()
				}
			}

			if ok {
				type pair struct{ u, p int64 }
				candidates := make([]pair, 0, len(likeIdx))
				seenPair := make(map[pair]bool, len(likeIdx))
				for _, idx := range likeIdx {
					j := jobs[idx]
					if !exists[j.postID] {
						results[idx].notFound = true
						continue
					}
					p := pair{j.userID, j.postID}
					if seenPair[p] {
						continue
					}
					seenPair[p] = true
					candidates = append(candidates, p)
				}

				inserted := make(map[pair]bool, len(candidates))
				if len(candidates) > 0 {
					in := make([]likeIn, 0, len(candidates))
					for _, p := range candidates {
						in = append(in, likeIn{U: p.u, P: p.p})
					}
					buf, err := json.Marshal(in)
					if err != nil {
						ok = false
					} else {
						rows, err := a.insertLikesBulk.QueryContext(ctx, string(buf))
						if err != nil {
							ok = false
						} else {
							for rows.Next() {
								var u, p int64
								if err := rows.Scan(&u, &p); err != nil {
									ok = false
									break
								}
								inserted[pair{u, p}] = true
							}
							if err := rows.Err(); err != nil {
								ok = false
							}
							rows.Close()
						}
					}
				}
				if ok {
					for _, idx := range likeIdx {
						j := jobs[idx]
						if results[idx].notFound {
							continue
						}
						p := pair{j.userID, j.postID}
						if inserted[p] {
							results[idx].already = false
							delete(inserted, p)
						} else {
							results[idx].already = true
						}
					}
				}
			}
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
