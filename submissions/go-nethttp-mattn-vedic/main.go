package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"database/sql/driver"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"hash"
	"io"
	"log"
	"math"
	"net"
	"net/http"
	"net/http/pprof"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/mattn/go-sqlite3"
)

type post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}
type tokenPayload struct {
	Sub      string   `json:"sub"`
	Username any      `json:"username"`
	Exp      *float64 `json:"exp"`
}
type tokenHeader struct {
	Alg string `json:"alg"`
}

var readerPool *sqlitePool
var writerPool *sqlitePool
var secret []byte
var authScratchPool = sync.Pool{New: func() any {
	return &authScratch{mac: hmac.New(sha256.New, secret)}
}}
var jwtDot = [...]byte{'.'}
var start = time.Now()
var feedJSONQuery = `SELECT json_object('posts', json_group_array(json_object(
	'id', id, 'body', body, 'created_at', created_at, 'author', username, 'like_count', like_count
))) FROM (
	SELECT p.id, p.body, p.created_at, u.username,
		(SELECT count(*) FROM likes l WHERE l.post_id=p.id) AS like_count
	FROM posts p JOIN users u ON u.id=p.user_id
	ORDER BY p.created_at DESC, p.id DESC LIMIT 20
)`
var postByIDJSONQuery = `SELECT json_object('post', json_object(
	'id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
	'like_count', (SELECT count(*) FROM likes l WHERE l.post_id=p.id)
)) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?`
var idPattern = regexp.MustCompile(`^[0-9]+$`)
var errNoRows = errors.New("no rows")

type sqliteConn struct {
	conn       *sqlite3.SQLiteConn
	jsonBuf    []byte
	health     driver.Stmt
	feed       driver.Stmt
	postByID   driver.Stmt
	createPost driver.Stmt
	createLike driver.Stmt
	postExists driver.Stmt
}

type sqlitePool struct {
	available chan *sqliteConn
	all       []*sqliteConn
}

type authScratch struct {
	header  []byte
	payload []byte
	sig     []byte
	sum     []byte
	mac     hash.Hash
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		writeError(w, 500, "internal server error")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(b)
}
func writeRawJSON(w http.ResponseWriter, status int, body []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, struct {
		Error string `json:"error"`
	}{msg})
}
func parseID(s string) (int64, bool) {
	if !idPattern.MatchString(s) {
		return 0, false
	}
	n, e := strconv.ParseInt(s, 10, 64)
	return n, e == nil && n > 0
}
func parseIDBytes(s []byte) (int64, bool) {
	if len(s) == 0 {
		return 0, false
	}
	var n int64
	for _, c := range s {
		if c < '0' || c > '9' || n > (math.MaxInt64-int64(c-'0'))/10 {
			return 0, false
		}
		n = n*10 + int64(c-'0')
	}
	return n, n > 0
}
func errorStatus(w http.ResponseWriter, err error) {
	if errors.Is(err, errNoRows) {
		writeError(w, 404, "post not found")
	} else {
		log.Printf("database error: %v", err)
		writeError(w, 500, "internal server error")
	}
}

func queryOne(stmt driver.Stmt, args []driver.Value, dest []driver.Value) error {
	rows, err := stmt.Query(args)
	if err != nil {
		return err
	}
	defer rows.Close()
	if err = rows.Next(dest); errors.Is(err, io.EOF) {
		return errNoRows
	}
	return err
}

func queryText(conn *sqliteConn, stmt driver.Stmt, args []driver.Value) ([]byte, error) {
	textStmt, ok := stmt.(*sqlite3.SQLiteStmt)
	if !ok {
		return nil, errors.New("SQLite driver does not support fused text queries")
	}
	conn.jsonBuf = conn.jsonBuf[:0]
	body, found, err := textStmt.QueryText(conn.jsonBuf, args)
	if err != nil {
		return nil, err
	}
	conn.jsonBuf = body
	if !found {
		return nil, errNoRows
	}
	return conn.jsonBuf, err
}

func queryFeed(conn *sqliteConn) ([]byte, error) {
	return queryText(conn, conn.feed, nil)
}

func authenticate(h []byte) (int64, string, int, string) {
	if !bytes.HasPrefix(h, []byte("Bearer ")) {
		return 0, "", 401, "missing bearer token"
	}
	token := h[len("Bearer "):]
	first := bytes.IndexByte(token, '.')
	if first < 0 {
		return 0, "", 401, "invalid or expired token"
	}
	rest := token[first+1:]
	second := bytes.IndexByte(rest, '.')
	if second < 0 || bytes.IndexByte(rest[second+1:], '.') >= 0 {
		return 0, "", 401, "invalid or expired token"
	}
	headerPart, payloadPart, sigPart := token[:first], rest[:second], rest[second+1:]
	scratch := authScratchPool.Get().(*authScratch)
	defer authScratchPool.Put(scratch)
	var e1, e2, e3 error
	scratch.header, e1 = decodeBase64(scratch.header, headerPart)
	scratch.payload, e2 = decodeBase64(scratch.payload, payloadPart)
	scratch.sig, e3 = decodeBase64(scratch.sig, sigPart)
	if e1 != nil || e2 != nil || e3 != nil {
		return 0, "", 401, "invalid or expired token"
	}
	var head tokenHeader
	if json.Unmarshal(scratch.header, &head) != nil || head.Alg != "HS256" {
		return 0, "", 401, "invalid or expired token"
	}
	scratch.mac.Reset()
	_, _ = scratch.mac.Write(headerPart)
	_, _ = scratch.mac.Write(jwtDot[:])
	_, _ = scratch.mac.Write(payloadPart)
	scratch.sum = scratch.mac.Sum(scratch.sum[:0])
	if !hmac.Equal(scratch.sig, scratch.sum) {
		return 0, "", 401, "invalid or expired token"
	}
	var p tokenPayload
	if json.Unmarshal(scratch.payload, &p) != nil {
		return 0, "", 401, "invalid or expired token"
	}
	if p.Exp != nil && (math.IsNaN(*p.Exp) || float64(time.Now().Unix()) >= *p.Exp) {
		return 0, "", 401, "invalid or expired token"
	}
	id, ok := parseID(p.Sub)
	if !ok {
		return 0, "", 401, "invalid token payload"
	}
	name, ok := p.Username.(string)
	if !ok {
		return 0, "", 401, "invalid token payload"
	}
	return id, name, 0, ""
}

func decodeBase64(dst, src []byte) ([]byte, error) {
	need := base64.RawURLEncoding.DecodedLen(len(src))
	if cap(dst) < need {
		dst = make([]byte, need)
	} else {
		dst = dst[:need]
	}
	n, err := base64.RawURLEncoding.Decode(dst, src)
	return dst[:n], err
}

func handler(w http.ResponseWriter, method, path, auth, body []byte) {
	switch {
	case bytes.Equal(path, []byte("/health")) && bytes.Equal(method, []byte("GET")):
		conn := readerPool.acquire()
		dest := make([]driver.Value, 1)
		e := queryOne(conn.health, nil, dest)
		readerPool.release(conn)
		if e != nil {
			writeJSON(w, 503, map[string]any{"status": "degraded", "db": "unreachable", "error": e.Error()})
			return
		}
		writeJSON(w, 200, struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Uptime int64  `json:"uptime_s"`
		}{"ok", "ok", int64(time.Since(start).Seconds())})
	case bytes.Equal(path, []byte("/feed")) && bytes.Equal(method, []byte("GET")):
		conn := readerPool.acquire()
		body, e := queryFeed(conn)
		if e != nil {
			readerPool.release(conn)
			errorStatus(w, e)
			return
		}
		writeRawJSON(w, 200, body)
		readerPool.release(conn)
	case bytes.HasPrefix(path, []byte("/posts/")) && bytes.Equal(method, []byte("GET")):
		id, ok := parseIDBytes(bytes.TrimPrefix(path, []byte("/posts/")))
		if !ok {
			writeError(w, 400, "invalid post id")
			return
		}
		conn := readerPool.acquire()
		body, e := queryText(conn, conn.postByID, []driver.Value{id})
		if e != nil {
			readerPool.release(conn)
			errorStatus(w, e)
			return
		}
		writeRawJSON(w, 200, body)
		readerPool.release(conn)
	case bytes.Equal(path, []byte("/posts")) && bytes.Equal(method, []byte("POST")):
		uid, name, status, msg := authenticate(auth)
		if status != 0 {
			writeError(w, status, msg)
			return
		}
		if len(body) > 16385 {
			body = body[:16385]
		}
		var data map[string]json.RawMessage
		if json.Unmarshal(body, &data) != nil {
			writeError(w, 400, "malformed JSON body")
			return
		}
		var postBody string
		if raw, ok := data["body"]; !ok || json.Unmarshal(raw, &postBody) != nil {
			writeError(w, 400, "body is required")
			return
		}
		postBody = strings.TrimSpace(postBody)
		if postBody == "" {
			writeError(w, 400, "body is required")
			return
		}
		if utf8.RuneCountInString(postBody) > 500 {
			writeError(w, 400, "body must be at most 500 characters")
			return
		}
		var p post
		p.Body = postBody
		p.Author = name
		conn := writerPool.acquire()
		rows, e := conn.createPost.Query([]driver.Value{uid, postBody})
		if e == nil {
			dest := make([]driver.Value, 2)
			e = rows.Next(dest)
			_ = rows.Close()
			if e == nil {
				var valid bool
				p.ID, valid = dest[0].(int64)
				if !valid {
					e = fmt.Errorf("SQLite returned %T for inserted post id", dest[0])
				} else if p.CreatedAt, valid = dest[1].(string); !valid {
					e = fmt.Errorf("SQLite returned %T for inserted post timestamp", dest[1])
				}
			}
		}
		writerPool.release(conn)
		if e != nil {
			errorStatus(w, e)
			return
		}
		writeJSON(w, 201, struct {
			Post post `json:"post"`
		}{p})
	case bytes.HasPrefix(path, []byte("/posts/")) && bytes.HasSuffix(path, []byte("/like")) && bytes.Equal(method, []byte("POST")):
		uid, _, status, msg := authenticate(auth)
		if status != 0 {
			writeError(w, status, msg)
			return
		}
		raw := bytes.TrimSuffix(bytes.TrimPrefix(path, []byte("/posts/")), []byte("/like"))
		raw = bytes.TrimSuffix(raw, []byte("/"))
		id, ok := parseIDBytes(raw)
		if !ok {
			writeError(w, 400, "invalid post id")
			return
		}
		conn := writerPool.acquire()
		res, e := conn.createLike.Exec([]driver.Value{uid, id, id})
		if e != nil {
			writerPool.release(conn)
			errorStatus(w, e)
			return
		}
		n, _ := res.RowsAffected()
		if n == 0 {
			dest := make([]driver.Value, 1)
			e = queryOne(conn.postExists, []driver.Value{id}, dest)
		}
		writerPool.release(conn)
		if e != nil {
			errorStatus(w, e)
			return
		}
		if n == 1 {
			writeJSON(w, 201, struct {
				Liked   bool  `json:"liked"`
				Already bool  `json:"already_liked"`
				ID      int64 `json:"post_id"`
			}{true, false, id})
			return
		}
		writeJSON(w, 200, struct {
			Liked   bool  `json:"liked"`
			Already bool  `json:"already_liked"`
			ID      int64 `json:"post_id"`
		}{true, true, id})
	default:
		writeError(w, 404, "not found")
	}
}

func startProfiler() error {
	addr := strings.TrimSpace(os.Getenv("PPROF_ADDR"))
	if addr == "" {
		return nil
	}

	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("start pprof listener: %w", err)
	}
	tcpAddr, ok := listener.Addr().(*net.TCPAddr)
	if !ok || !tcpAddr.IP.IsLoopback() {
		_ = listener.Close()
		return fmt.Errorf("PPROF_ADDR must resolve to a loopback address")
	}

	profiler := http.NewServeMux()
	profiler.HandleFunc("/debug/pprof/", pprof.Index)
	profiler.HandleFunc("/debug/pprof/cmdline", pprof.Cmdline)
	profiler.HandleFunc("/debug/pprof/profile", pprof.Profile)
	profiler.HandleFunc("/debug/pprof/symbol", pprof.Symbol)
	profiler.HandleFunc("/debug/pprof/trace", pprof.Trace)
	for _, name := range []string{"allocs", "block", "goroutine", "heap", "mutex", "threadcreate"} {
		profiler.Handle("/debug/pprof/"+name, pprof.Handler(name))
	}
	go func() {
		if err := http.Serve(listener, profiler); err != nil {
			log.Printf("pprof server stopped: %v", err)
		}
	}()
	log.Printf("pprof listening on http://%s/debug/pprof/", listener.Addr())
	return nil
}

func poolLimit(name string, defaultValue int) (int, error) {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return defaultValue, nil
	}
	limit, err := strconv.Atoi(value)
	if err != nil || limit < 1 {
		return 0, fmt.Errorf("%s must be a positive integer", name)
	}
	return limit, nil
}

func (c *sqliteConn) prepare(reader bool) error {
	var err error
	if reader {
		if c.health, err = c.conn.Prepare("SELECT 1"); err == nil {
			c.feed, err = c.conn.Prepare(feedJSONQuery)
		}
		if err == nil {
			c.postByID, err = c.conn.Prepare(postByIDJSONQuery)
		}
		if err == nil {
			c.postExists, err = c.conn.Prepare("SELECT 1 FROM posts WHERE id=?")
		}
	} else {
		if c.createPost, err = c.conn.Prepare("INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at"); err == nil {
			c.createLike, err = c.conn.Prepare(`INSERT INTO likes(user_id,post_id) SELECT ?,? WHERE EXISTS(SELECT 1 FROM posts WHERE id=?) ON CONFLICT(user_id,post_id) DO NOTHING`)
		}
		if err == nil {
			c.postExists, err = c.conn.Prepare("SELECT 1 FROM posts WHERE id=?")
		}
	}
	return err
}

func (c *sqliteConn) close() {
	for _, stmt := range []driver.Stmt{c.health, c.feed, c.postByID, c.createPost, c.createLike, c.postExists} {
		if stmt != nil {
			_ = stmt.Close()
		}
	}
	if c.conn != nil {
		_ = c.conn.Close()
	}
}

func (p *sqlitePool) acquire() *sqliteConn     { return <-p.available }
func (p *sqlitePool) release(conn *sqliteConn) { p.available <- conn }
func (p *sqlitePool) close() {
	for _, conn := range p.all {
		conn.close()
	}
}

func openPool(size int, dsn string, reader bool) (*sqlitePool, error) {
	p := &sqlitePool{available: make(chan *sqliteConn, size), all: make([]*sqliteConn, 0, size)}
	sqliteDriver := &sqlite3.SQLiteDriver{}
	for i := 0; i < size; i++ {
		opened, err := sqliteDriver.Open(dsn)
		if err != nil {
			p.close()
			return nil, err
		}
		conn := &sqliteConn{conn: opened.(*sqlite3.SQLiteConn)}
		if err = conn.prepare(reader); err != nil {
			conn.close()
			p.close()
			return nil, err
		}
		if reader {
			dest := make([]driver.Value, 1)
			if err = queryOne(conn.health, nil, dest); err != nil {
				conn.close()
				p.close()
				return nil, err
			}
		}
		p.all = append(p.all, conn)
		p.available <- conn
	}
	return p, nil
}

func openDatabasePools(path string, readerLimit, writerLimit int) error {
	baseDSN := fmt.Sprintf("file:%s?_busy_timeout=5000&_cache_size=-65536&_mmap_size=1073741824", path)
	var err error
	writerPool, err = openPool(writerLimit, baseDSN+"&_journal_mode=WAL&_synchronous=NORMAL", false)
	if err != nil {
		return fmt.Errorf("open SQLite writer pool: %w", err)
	}
	readerPool, err = openPool(readerLimit, baseDSN+"&mode=ro&_query_only=1", true)
	if err != nil {
		writerPool.close()
		return fmt.Errorf("open SQLite reader pool: %w", err)
	}
	return nil
}

func main() {
	path := os.Getenv("SQLITE_PATH")
	if path == "" {
		log.Fatal("SQLITE_PATH is required")
	}
	secret = []byte(os.Getenv("JWT_SECRET"))
	if len(secret) == 0 {
		log.Fatal("JWT_SECRET is required")
	}
	workers, err := poolLimit("HTTP_WORKERS", 1)
	if err != nil {
		log.Fatal(err)
	}
	handlerWorkers = workers
	readerLimit, err := poolLimit("DB_READERS", handlerWorkers)
	if err != nil {
		log.Fatal(err)
	}
	writerLimit, err := poolLimit("DB_WRITERS", 1)
	if err != nil {
		log.Fatal(err)
	}
	if err = openDatabasePools(path, readerLimit, writerLimit); err != nil {
		log.Fatal(err)
	}
	inline := handlerWorkers == 1
	log.Printf("Server pools ready: workers=%d readers=%d writers=%d inline=%t", handlerWorkers, readerLimit, writerLimit, inline)
	if err = startProfiler(); err != nil {
		log.Fatal(err)
	}
	host := os.Getenv("HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "3000"
	}
	log.Fatal(serveEpoll(host, port, handler, inline))
}
