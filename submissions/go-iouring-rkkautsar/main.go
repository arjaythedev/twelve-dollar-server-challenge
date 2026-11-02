// A minimal feed API: net/http parser + direct SQLite (cgo).
package main

import (
	"bufio"
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	_ "net/http/pprof"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unicode/utf8"
)

type postRow struct {
	ID        int64
	Body      []byte
	CreatedAt []byte
	Author    []byte
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
	post     postRow
	already  bool
	notFound bool
	failed   bool
}

type app struct {
	secret  []byte
	started time.Time

	reads chan *readConn
	write *writeConn
	dbMu  sync.Mutex

	writeCh     chan *writeJob
	batchWindow time.Duration

	readsGroup readGroup
	readGen    atomic.Uint64
	readWindow time.Duration
	coalesce   bool
}

// readCall is one in-flight SQLite read. Concurrent requests for the same key
// wait for the same call; the call is removed as soon as it finishes, so this
// is request coalescing, not a cross-request cache.
type readResp struct {
	status int
	body   []byte
	packet []byte
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

	// Give concurrent requests a short window to join this read. On one vCPU
	// the leader would otherwise finish before another goroutine gets to run.
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
	path := os.Getenv("SQLITE_PATH")

	shards := 3
	if v := os.Getenv("SHARDS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 && n <= 8 {
			shards = n
		}
	}
	layout := muxMixed
	switch strings.ToLower(os.Getenv("MUX_LAYOUT")) {
	case "", "mixed":
	case "read", "readwrite", "read-shared-write":
		layout = muxReadSharedWrite
	case "feedpost", "feed-post", "feedpost-shared-write":
		layout = muxFeedPostSharedWrite
	default:
		log.Fatalf("unknown MUX_LAYOUT %q", os.Getenv("MUX_LAYOUT"))
	}

	readConns := 2
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
	reads := make(chan *readConn, readConns)
	for range readConns {
		rc, err := newReadConn(path)
		if err != nil {
			log.Fatal(err)
		}
		reads <- rc
	}
	wc, err := newWriteConn(path)
	if err != nil {
		log.Fatal(err)
	}

	windowUS := 500
	if v := os.Getenv("READ_BATCH_US"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			windowUS = n
		}
	}
	batchMS := 100
	if v := os.Getenv("WRITE_BATCH_MS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n >= 0 {
			batchMS = n
		}
	}

	a := &app{
		secret:      []byte(os.Getenv("JWT_SECRET")),
		started:     time.Now(),
		reads:       reads,
		write:       wc,
		writeCh:     make(chan *writeJob, 256),
		batchWindow: time.Duration(batchMS) * time.Millisecond,
		readWindow:  time.Duration(windowUS) * time.Microsecond,
		coalesce:    coalesce,
		readsGroup:  readGroup{enabled: coalesce},
	}
	go a.writeLoop()
	// Opt-in profiling listener; unused in the benchmark runs.
	if addr := os.Getenv("PPROF_ADDR"); addr != "" {
		go func() {
			log.Println("pprof listening on", addr)
			log.Println(http.ListenAndServe(addr, nil))
		}()
	}

	// The raw io_uring + DB-owner path is the default. URING=0 opts out and
	// exercises the goroutine/serveConn fallback.
	if os.Getenv("URING") != "0" {
		feed := func() []byte {
			r := a.loadFeed()
			if r.packet != nil {
				return r.packet
			}
			return encodeMuxResponse(&capture{status: r.status, body: r.body}, false)
		}
		postBatch := func(ids []int64) map[int64][]byte {
			res := a.loadPosts(ids)
			out := make(map[int64][]byte, len(ids))
			for _, id := range ids {
				r, ok := res[id]
				if !ok || r.packet == nil {
					out[id] = encodeMuxResponse(&capture{status: 500, body: []byte(`{"error":"internal server error"}`)}, false)
					continue
				}
				out[id] = r.packet
			}
			return out
		}
		writeBatch := func(reqs []*httpReq) []readResp {
			return a.muxWriteBatch(reqs)
		}
		var writeJobs chan *muxJob
		if layout != muxMixed {
			writeJobs = make(chan *muxJob, 256)
			go runWriteWorker(writeJobs, writeBatch)
		}
		newMux := func() (*uringMux, error) {
			return newUringMux(func(req *httpReq) ([]byte, bool) {
				w := &capture{status: 200}
				a.ServeHTTP(w, req)
				closeAfter := req.close
				return encodeMuxResponse(w, closeAfter), closeAfter
			}, feed, postBatch, writeBatch, a.coalesce, 1, layout, writeJobs)
		}
		started := 0
		for range shards {
			mux, err := newMux()
			if err != nil {
				log.Printf("raw io_uring mux unavailable: %v", err)
				continue
			}
			started++
			go func(m *uringMux) {
				if err := m.serve(host, port); err != nil {
					log.Printf("raw io_uring mux stopped: %v", err)
				}
			}(mux)
		}
		if started > 0 {
			log.Printf("raw io_uring request mux enabled (%d shards, layout %d)", started, layout)
			select {}
		}
	}

	ln, err := net.Listen("tcp", host+":"+port)
	if err != nil {
		log.Fatal(err)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			// EMFILE/ENFILE cannot make progress until a descriptor is
			// released. Retrying immediately spins at 100% CPU and floods the
			// log, so pause before retrying.
			if errors.Is(err, syscall.EMFILE) || errors.Is(err, syscall.ENFILE) {
				time.Sleep(20 * time.Millisecond)
			}
			log.Print(err)
			continue
		}
		go a.serveConn(c)
	}
}

// serveConn is a bufio connection loop. Coalesced reads arrive as a complete
// immutable packet and are written directly, so the per-connection write buffer
// only has to hold the small write/error responses.
func (a *app) serveConn(c net.Conn) {
	defer c.Close()
	if tc, ok := c.(*net.TCPConn); ok {
		tc.SetNoDelay(true)
	}
	br := bufio.NewReaderSize(c, 2048)
	bw := bufio.NewWriterSize(c, 1024)
	for {
		req, err := readReq(br)
		if err != nil {
			return
		}
		resp := &capture{status: http.StatusOK}
		a.ServeHTTP(resp, req)
		var werr error
		if resp.packet != nil && !req.close {
			// Packet already contains the status line, headers and body.
			_, werr = c.Write(resp.packet)
		} else {
			bw.WriteString("HTTP/1.1 ")
			bw.WriteString(strconv.Itoa(resp.status))
			bw.WriteByte(' ')
			bw.WriteString(http.StatusText(resp.status))
			bw.WriteString("\r\nContent-Type: application/json\r\nContent-Length: ")
			bw.WriteString(strconv.Itoa(len(resp.body)))
			if req.close {
				bw.WriteString("\r\nConnection: close")
			}
			bw.WriteString("\r\n\r\n")
			_, werr = bw.Write(resp.body)
			if werr == nil {
				werr = bw.Flush()
			}
		}
		if resp.buf != nil {
			bufPool.Put(resp.buf)
			resp.buf = nil
		}
		if werr != nil {
			return
		}
		if req.close {
			return
		}
	}
}

type capture struct {
	status int
	body   []byte
	packet []byte
	buf    *[]byte
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

func (a *app) applyBatch(jobs []*writeJob) ([]writeResult, bool) {
	a.dbMu.Lock()
	defer a.dbMu.Unlock()
	w := a.write
	if err := w.db.exec("BEGIN IMMEDIATE"); err != nil {
		return nil, false
	}

	results := make([]writeResult, len(jobs))
	ok := true
run:
	for i, j := range jobs {
		switch j.kind {
		case writeCreate:
			st := w.insertPost
			st.reset()
			if err := st.bindInt64(1, j.userID); err != nil {
				ok = false
				break run
			}
			if err := st.bindText(2, j.body); err != nil {
				ok = false
				break run
			}
			row, err := st.step()
			if err != nil || !row {
				ok = false
				break run
			}
			id := st.columnInt64(0)
			created := append([]byte(nil), st.columnText(1)...)
			if row, err = st.step(); err != nil || row {
				ok = false
				break run
			}
			st.reset()
			results[i].post = postRow{
				ID: id, Body: []byte(j.body), CreatedAt: created, Author: []byte(j.username),
			}
		case writeLike:
			st := w.insertLike
			st.reset()
			if err := st.bindInt64(1, j.userID); err != nil {
				ok = false
				break run
			}
			if err := st.bindInt64(2, j.postID); err != nil {
				ok = false
				break run
			}
			row, err := st.step()
			if err != nil || row {
				ok = false
				break run
			}
			changed := st.changes()
			st.reset()
			if changed > 0 {
				results[i].already = false
				continue
			}
			// Zero rows means duplicate like or missing post; only the
			// fallback needs the separate existence check.
			ex := w.postExists
			ex.reset()
			if err := ex.bindInt64(1, j.postID); err != nil {
				ok = false
				break run
			}
			row, err = ex.step()
			if err != nil {
				ok = false
				break run
			}
			ex.reset()
			if !row {
				results[i].notFound = true
				continue
			}
			results[i].already = true
		}
	}

	if ok {
		ok = w.db.exec("COMMIT") == nil
	}
	if !ok {
		w.insertPost.reset()
		w.postExists.reset()
		w.insertLike.reset()
		w.db.exec("ROLLBACK")
		return nil, false
	}
	// Do not let a request that starts after this commit join a read that
	// started before it. The generation is part of every coalescing key.
	a.readGen.Add(1)
	return results, true
}

func (a *app) runBatch(jobs []*writeJob) {
	results, ok := a.applyBatch(jobs)
	if !ok {
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

type httpReq struct {
	method string
	path   string
	auth   string
	body   []byte
	close  bool
}

func readReq(br *bufio.Reader) (*httpReq, error) {
	line, err := br.ReadSlice('\n')
	if err != nil {
		return nil, err
	}
	line = bytes.TrimSuffix(line, []byte("\n"))
	line = bytes.TrimSuffix(line, []byte("\r"))
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 < 0 {
		return nil, errBadRequest
	}
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 < 0 {
		return nil, errBadRequest
	}
	target := rest[:sp2]
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	version := rest[sp2+1:]
	req := &httpReq{
		method: string(line[:sp1]),
		path:   string(target),
		close:  bytes.Equal(version, []byte("HTTP/1.0")),
	}
	clen := 0
	for {
		l, err := br.ReadSlice('\n')
		if err != nil {
			return nil, err
		}
		l = bytes.TrimSuffix(l, []byte("\n"))
		l = bytes.TrimSuffix(l, []byte("\r"))
		if len(l) == 0 {
			break
		}
		colon := bytes.IndexByte(l, ':')
		if colon < 0 {
			continue
		}
		name := bytes.TrimSpace(l[:colon])
		value := bytes.TrimSpace(l[colon+1:])
		switch {
		case bytes.EqualFold(name, []byte("authorization")):
			req.auth = string(value)
		case bytes.EqualFold(name, []byte("content-length")):
			n, err := strconv.Atoi(string(value))
			if err != nil || n < 0 {
				return nil, errBadRequest
			}
			clen = n
		case bytes.EqualFold(name, []byte("connection")):
			if bytes.Contains(bytes.ToLower(value), []byte("close")) {
				req.close = true
			}
		}
	}
	if clen > 0 {
		req.body = make([]byte, clen)
		if _, err := io.ReadFull(br, req.body); err != nil {
			return nil, err
		}
	}
	return req, nil
}

var errBadRequest = errors.New("bad request")

func (a *app) ServeHTTP(w http.ResponseWriter, r *httpReq) {
	switch p := r.path; p {
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

func (a *app) health(w http.ResponseWriter, r *httpReq) {
	rc := <-a.reads
	st := rc.health
	st.reset()
	row, err := st.step()
	st.reset()
	a.reads <- rc

	bp := bufPool.Get().(*[]byte)
	b := (*bp)[:0]
	if err != nil || !row {
		b = append(b, `{"status":"degraded","db":"unreachable","error":`...)
		b = appendString(b, "database unavailable")
		b = append(b, '}')
		*bp = b
		writeBody(w, http.StatusServiceUnavailable, bp, b)
		return
	}
	b = append(b, `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started).Seconds()), 10)
	b = append(b, '}')
	*bp = b
	writeBody(w, http.StatusOK, bp, b)
}

func (a *app) feedPosts(w http.ResponseWriter, r *httpReq) {
	key := "feed:" + strconv.FormatUint(a.readGen.Load(), 10)
	writeSharedBody(w, a.readsGroup.do(key, a.readWindow, a.loadFeed))
}

// loadFeed performs the one SQLite read that serves a coalesced group of feed
// requests. The returned body is immutable and shared by that group only.
func (a *app) loadFeed() readResp {
	rc := <-a.reads
	st := rc.feed
	st.reset()
	row, err := st.step()
	if err != nil {
		st.reset()
		a.reads <- rc
		return readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
	}
	b := make([]byte, 0, 4096)
	b = append(b, `{"posts":`...)
	if row {
		b = append(b, st.columnText(0)...)
	} else {
		b = append(b, `[]`...)
	}
	b = append(b, '}')
	st.reset()
	a.reads <- rc
	return readResp{status: http.StatusOK, body: b, packet: makePacket(http.StatusOK, b)}
}

func (a *app) getPostByID(w http.ResponseWriter, r *httpReq, idStr string) {
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	key := "post:" + strconv.FormatUint(a.readGen.Load(), 10) + ":" + strconv.FormatInt(id, 10)
	writeSharedBody(w, a.readsGroup.do(key, a.readWindow, func() readResp {
		return a.loadPost(id)
	}))
}

// loadPost serves one coalesced group of concurrent reads for the same post.
func (a *app) loadPost(id int64) readResp {
	rc := <-a.reads
	st := rc.getPost
	st.reset()
	if err := st.bindInt64(1, id); err != nil {
		st.reset()
		a.reads <- rc
		return readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
	}
	row, err := st.step()
	if err != nil {
		st.reset()
		a.reads <- rc
		return readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
	}
	if !row {
		st.reset()
		a.reads <- rc
		return readResp{status: http.StatusNotFound, body: []byte(`{"error":"post not found"}`)}
	}
	b := append([]byte(nil), st.columnText(0)...)
	st.reset()
	a.reads <- rc
	return readResp{status: http.StatusOK, body: b, packet: makePacket(http.StatusOK, b)}
}

// loadPosts serves a whole batch of GET /posts/:id requests with one IN query.
func (a *app) loadPosts(ids []int64) map[int64]readResp {
	if len(ids) == 0 {
		return nil
	}
	unique := make([]int64, 0, len(ids))
	seen := make(map[int64]struct{}, len(ids))
	for _, id := range ids {
		if _, ok := seen[id]; ok {
			continue
		}
		seen[id] = struct{}{}
		unique = append(unique, id)
	}
	sort.Slice(unique, func(i, j int) bool { return unique[i] < unique[j] })
	rc := <-a.reads
	n := len(unique)
	st, ok := rc.postStmts[n]
	if !ok {
		q := strings.Replace(getPostSQL, "SELECT json_object", "SELECT p.id, json_object", 1)
		ph := strings.TrimSuffix(strings.Repeat("?,", n), ",")
		q = strings.Replace(q, "WHERE p.id = ?", "WHERE p.id IN ("+ph+")", 1)
		var err error
		st, err = rc.db.prepare(q)
		if err != nil {
			a.reads <- rc
			return nil
		}
		rc.postStmts[n] = st
	}
	st.reset()
	out := make(map[int64]readResp, len(unique))
	for i, id := range unique {
		if err := st.bindInt64(i+1, id); err != nil {
			st.reset()
			a.reads <- rc
			return nil
		}
	}
	hadErr := false
	for {
		row, err := st.step()
		if err != nil {
			hadErr = true
			break
		}
		if !row {
			break
		}
		id := st.columnInt64(0)
		b := append([]byte(nil), st.columnText(1)...)
		out[id] = readResp{status: http.StatusOK, body: b, packet: makePacket(http.StatusOK, b)}
	}
	st.reset()
	a.reads <- rc
	for _, id := range ids {
		if _, ok := out[id]; ok {
			continue
		}
		if hadErr {
			out[id] = readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`), packet: makePacket(http.StatusInternalServerError, []byte(`{"error":"internal server error"}`))}
			continue
		}
		out[id] = readResp{status: http.StatusNotFound, body: []byte(`{"error":"post not found"}`), packet: makePacket(http.StatusNotFound, []byte(`{"error":"post not found"}`))}
	}
	return out
}

func (a *app) prepareCreatePost(r *httpReq) (*writeJob, *capture) {
	w := &capture{status: http.StatusCreated}
	userID, username, authErr := a.authenticate(r)
	if authErr != "" {
		writeError(w, http.StatusUnauthorized, authErr)
		return nil, w
	}
	var payload struct {
		Body json.RawMessage `json:"body"`
	}
	if err := json.Unmarshal(r.body, &payload); err != nil {
		writeError(w, http.StatusBadRequest, "malformed JSON body")
		return nil, w
	}
	var body string
	if payload.Body == nil || json.Unmarshal(payload.Body, &body) != nil {
		writeError(w, http.StatusBadRequest, "body is required")
		return nil, w
	}
	body = strings.TrimSpace(body)
	if body == "" {
		writeError(w, http.StatusBadRequest, "body is required")
		return nil, w
	}
	if utf8.RuneCountInString(body) > 500 {
		writeError(w, http.StatusBadRequest, "body must be at most 500 characters")
		return nil, w
	}
	return &writeJob{kind: writeCreate, userID: userID, username: username, body: body, reply: make(chan writeResult, 1)}, nil
}

func (a *app) prepareLike(r *httpReq, idStr string) (*writeJob, *capture) {
	w := &capture{status: http.StatusCreated}
	userID, _, authErr := a.authenticate(r)
	if authErr != "" {
		writeError(w, http.StatusUnauthorized, authErr)
		return nil, w
	}
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return nil, w
	}
	return &writeJob{kind: writeLike, userID: userID, postID: id, reply: make(chan writeResult, 1)}, nil
}

func createResponse(result writeResult) (int, []byte) {
	if result.failed {
		return http.StatusInternalServerError, []byte(`{"error":"internal server error"}`)
	}
	b := make([]byte, 0, 256)
	b = append(b, `{"post":`...)
	b = appendPostRow(b, result.post)
	b = append(b, '}')
	return http.StatusCreated, b
}

func likeResponse(result writeResult, id int64) (int, []byte) {
	if result.failed {
		return http.StatusInternalServerError, []byte(`{"error":"internal server error"}`)
	}
	if result.notFound {
		return http.StatusNotFound, []byte(`{"error":"post not found"}`)
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
	return status, b
}

// muxWriteBatch parses and commits a batch of writes in one transaction.
func (a *app) muxWriteBatch(reqs []*httpReq) []readResp {
	out := make([]readResp, len(reqs))
	jobs := make([]*writeJob, 0, len(reqs))
	slots := make([]int, 0, len(reqs))
	for i, req := range reqs {
		var job *writeJob
		var errResp *capture
		if req.path == "/posts" {
			job, errResp = a.prepareCreatePost(req)
		} else {
			idStr, ok := strings.CutSuffix(strings.TrimPrefix(req.path, "/posts/"), "/like")
			if !ok {
				errResp = &capture{status: http.StatusNotFound, body: []byte(`{"error":"not found"}`)}
			} else {
				job, errResp = a.prepareLike(req, idStr)
			}
		}
		if job == nil {
			out[i] = readResp{status: errResp.status, body: errResp.body}
			continue
		}
		jobs = append(jobs, job)
		slots = append(slots, i)
	}
	if len(jobs) > 0 {
		results, ok := a.applyBatch(jobs)
		for k, job := range jobs {
			if !ok {
				out[slots[k]] = readResp{status: http.StatusInternalServerError, body: []byte(`{"error":"internal server error"}`)}
				continue
			}
			if job.kind == writeCreate {
				status, body := createResponse(results[k])
				out[slots[k]] = readResp{status: status, body: body}
			} else {
				status, body := likeResponse(results[k], job.postID)
				out[slots[k]] = readResp{status: status, body: body}
			}
		}
	}
	for i := range out {
		if out[i].packet == nil {
			out[i].packet = makePacket(out[i].status, out[i].body)
		}
	}
	return out
}

func (a *app) createPost(w http.ResponseWriter, r *httpReq) {
	job, errResp := a.prepareCreatePost(r)
	if job == nil {
		w.WriteHeader(errResp.status)
		w.Write(errResp.body)
		return
	}
	a.writeCh <- job
	result := <-job.reply
	status, body := createResponse(result)
	w.WriteHeader(status)
	w.Write(body)
}

func (a *app) likePost(w http.ResponseWriter, r *httpReq, idStr string) {
	job, errResp := a.prepareLike(r, idStr)
	if job == nil {
		w.WriteHeader(errResp.status)
		w.Write(errResp.body)
		return
	}
	a.writeCh <- job
	result := <-job.reply
	status, body := likeResponse(result, job.postID)
	w.WriteHeader(status)
	w.Write(body)
}

// authenticate verifies the bearer JWT and returns the user id and username.
// A non-empty third result is the error message to send with status 401.
func (a *app) authenticate(r *httpReq) (int64, string, string) {
	const (
		missing = "missing bearer token"
		invalid = "invalid or expired token"
		payload = "invalid token payload"
	)
	header := r.auth
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

func appendPostRow(dst []byte, p postRow) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, p.ID, 10)
	dst = append(dst, `,"body":`...)
	dst = appendStringBytes(dst, p.Body)
	dst = append(dst, `,"created_at":`...)
	dst = appendStringBytes(dst, p.CreatedAt)
	dst = append(dst, `,"author":`...)
	dst = appendStringBytes(dst, p.Author)
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, p.LikeCount, 10)
	return append(dst, '}')
}

func appendString(dst []byte, s string) []byte {
	return appendStringBytes(dst, []byte(s))
}

// appendStringBytes writes a JSON string. Non-ASCII is emitted as UTF-8 (the
// spec makes escaping optional); quotes, backslashes and control bytes are
// escaped.
func appendStringBytes(dst []byte, s []byte) []byte {
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
	writeBody(w, status, bp, b)
}

// writeSharedBody sends an immutable body shared by an in-flight read group.
// It is not returned to bufPool because other waiters may still be writing it.
func writeSharedBody(w http.ResponseWriter, r readResp) {
	w.WriteHeader(r.status)
	if c, ok := w.(*capture); ok {
		c.body = r.body
		c.packet = r.packet
		c.buf = nil
		return
	}
	w.Write(r.body)
}

func makePacket(status int, body []byte) []byte {
	p := make([]byte, 0, len(body)+128)
	p = append(p, "HTTP/1.1 "...)
	p = strconv.AppendInt(p, int64(status), 10)
	p = append(p, ' ')
	p = append(p, http.StatusText(status)...)
	p = append(p, "\r\nContent-Type: application/json\r\nContent-Length: "...)
	p = strconv.AppendInt(p, int64(len(body)), 10)
	p = append(p, "\r\n\r\n"...)
	p = append(p, body...)
	return p
}

func writeBody(w http.ResponseWriter, status int, bp *[]byte, body []byte) {
	w.WriteHeader(status)
	if c, ok := w.(*capture); ok {
		c.body = body
		c.buf = bp
		return
	}
	w.Write(body)
	if bp != nil {
		bufPool.Put(bp)
	}
}
