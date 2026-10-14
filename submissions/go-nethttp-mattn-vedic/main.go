package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
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
	"time"

	_ "github.com/mattn/go-sqlite3"
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

var readerDB *sql.DB
var writerDB *sql.DB
var statements dbStatements
var secret []byte
var start = time.Now()
var postSelect = `SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id`
var idPattern = regexp.MustCompile(`^[0-9]+$`)

type dbStatements struct {
	health     *sql.Stmt
	feed       *sql.Stmt
	postByID   *sql.Stmt
	createPost *sql.Stmt
	createLike *sql.Stmt
	postExists *sql.Stmt
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
func readPost(row *sql.Row) (post, error) {
	var p post
	err := row.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount)
	return p, err
}
func readPostRows(rows *sql.Rows) ([]post, error) {
	out := make([]post, 0, 20)
	for rows.Next() {
		var p post
		if e := rows.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount); e != nil {
			return nil, e
		}
		out = append(out, p)
	}
	return out, rows.Err()
}
func errorStatus(w http.ResponseWriter, err error) {
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, 404, "post not found")
	} else {
		log.Printf("database error: %v", err)
		writeError(w, 500, "internal server error")
	}
}

func authenticate(r *http.Request) (int64, string, int, string) {
	h := r.Header.Get("Authorization")
	if !strings.HasPrefix(h, "Bearer ") {
		return 0, "", 401, "missing bearer token"
	}
	parts := strings.Split(h[7:], ".")
	if len(parts) != 3 {
		return 0, "", 401, "invalid or expired token"
	}
	dec := base64.RawURLEncoding.DecodeString
	hb, e1 := dec(parts[0])
	pb, e2 := dec(parts[1])
	sig, e3 := dec(parts[2])
	if e1 != nil || e2 != nil || e3 != nil {
		return 0, "", 401, "invalid or expired token"
	}
	var head tokenHeader
	if json.Unmarshal(hb, &head) != nil || head.Alg != "HS256" {
		return 0, "", 401, "invalid or expired token"
	}
	mac := hmac.New(sha256.New, secret)
	_, _ = mac.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(sig, mac.Sum(nil)) {
		return 0, "", 401, "invalid or expired token"
	}
	var p tokenPayload
	if json.Unmarshal(pb, &p) != nil {
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
func handler(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	switch {
	case path == "/health" && r.Method == http.MethodGet:
		var one int
		if e := statements.health.QueryRow().Scan(&one); e != nil {
			writeJSON(w, 503, map[string]any{"status": "degraded", "db": "unreachable", "error": e.Error()})
			return
		}
		writeJSON(w, 200, struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Uptime int64  `json:"uptime_s"`
		}{"ok", "ok", int64(time.Since(start).Seconds())})
	case path == "/feed" && r.Method == http.MethodGet:
		rows, e := statements.feed.Query()
		if e != nil {
			errorStatus(w, e)
			return
		}
		defer rows.Close()
		ps, e := readPostRows(rows)
		if e != nil {
			errorStatus(w, e)
			return
		}
		writeJSON(w, 200, struct {
			Posts []post `json:"posts"`
		}{ps})
	case strings.HasPrefix(path, "/posts/") && r.Method == http.MethodGet:
		id, ok := parseID(strings.TrimPrefix(path, "/posts/"))
		if !ok {
			writeError(w, 400, "invalid post id")
			return
		}
		p, e := readPost(statements.postByID.QueryRow(id))
		if e != nil {
			errorStatus(w, e)
			return
		}
		writeJSON(w, 200, struct {
			Post post `json:"post"`
		}{p})
	case path == "/posts" && r.Method == http.MethodPost:
		uid, name, status, msg := authenticate(r)
		if status != 0 {
			writeError(w, status, msg)
			return
		}
		b, e := io.ReadAll(io.LimitReader(r.Body, 16385))
		if e != nil {
			writeError(w, 400, "malformed JSON body")
			return
		}
		var data map[string]any
		if json.Unmarshal(b, &data) != nil {
			writeError(w, 400, "malformed JSON body")
			return
		}
		body, ok := data["body"].(string)
		if !ok {
			writeError(w, 400, "body is required")
			return
		}
		body = strings.TrimSpace(body)
		if body == "" {
			writeError(w, 400, "body is required")
			return
		}
		if len([]rune(body)) > 500 {
			writeError(w, 400, "body must be at most 500 characters")
			return
		}
		var p post
		p.Body = body
		p.Author = name
		e = statements.createPost.QueryRow(uid, body).Scan(&p.ID, &p.CreatedAt)
		if e != nil {
			errorStatus(w, e)
			return
		}
		writeJSON(w, 201, struct {
			Post post `json:"post"`
		}{p})
	case strings.HasPrefix(path, "/posts/") && strings.HasSuffix(path, "/like") && r.Method == http.MethodPost:
		uid, _, status, msg := authenticate(r)
		if status != 0 {
			writeError(w, status, msg)
			return
		}
		raw := strings.TrimSuffix(strings.TrimPrefix(path, "/posts/"), "/like")
		raw = strings.TrimSuffix(raw, "/")
		id, ok := parseID(raw)
		if !ok {
			writeError(w, 400, "invalid post id")
			return
		}
		res, e := statements.createLike.Exec(uid, id, id)
		if e != nil {
			errorStatus(w, e)
			return
		}
		n, _ := res.RowsAffected()
		if n == 1 {
			writeJSON(w, 201, struct {
				Liked   bool  `json:"liked"`
				Already bool  `json:"already_liked"`
				ID      int64 `json:"post_id"`
			}{true, false, id})
			return
		}
		var exists int
		if e = statements.postExists.QueryRow(id).Scan(&exists); e != nil {
			errorStatus(w, e)
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

func openDatabasePools(path string, readerLimit, writerLimit int) error {
	baseDSN := fmt.Sprintf("file:%s?_busy_timeout=5000&_cache_size=-65536&_mmap_size=1073741824", path)
	var err error
	writerDB, err = sql.Open("sqlite3", baseDSN+"&_journal_mode=WAL&_synchronous=NORMAL")
	if err != nil {
		return err
	}
	writerDB.SetMaxOpenConns(writerLimit)
	writerDB.SetMaxIdleConns(writerLimit)
	if err = writerDB.Ping(); err != nil {
		_ = writerDB.Close()
		return fmt.Errorf("open SQLite writer pool: %w", err)
	}

	readerDB, err = sql.Open("sqlite3", baseDSN+"&mode=ro&_query_only=1")
	if err != nil {
		_ = writerDB.Close()
		return err
	}
	readerDB.SetMaxOpenConns(readerLimit)
	readerDB.SetMaxIdleConns(readerLimit)
	if err = readerDB.Ping(); err != nil {
		_ = readerDB.Close()
		_ = writerDB.Close()
		return fmt.Errorf("open SQLite reader pool: %w", err)
	}
	statements.health, err = readerDB.Prepare("SELECT 1")
	if err == nil {
		statements.feed, err = readerDB.Prepare(postSelect + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20")
	}
	if err == nil {
		statements.postByID, err = readerDB.Prepare(postSelect + " WHERE p.id=?")
	}
	if err == nil {
		statements.postExists, err = readerDB.Prepare("SELECT 1 FROM posts WHERE id=?")
	}
	if err == nil {
		statements.createPost, err = writerDB.Prepare("INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at")
	}
	if err == nil {
		statements.createLike, err = writerDB.Prepare(`INSERT INTO likes(user_id,post_id) SELECT ?,? WHERE EXISTS(SELECT 1 FROM posts WHERE id=?) ON CONFLICT(user_id,post_id) DO NOTHING`)
	}
	if err != nil {
		closeStatements()
		_ = readerDB.Close()
		_ = writerDB.Close()
		return fmt.Errorf("prepare SQLite statements: %w", err)
	}
	return nil
}

func closeStatements() {
	for _, stmt := range []*sql.Stmt{statements.health, statements.feed, statements.postByID, statements.postExists, statements.createPost, statements.createLike} {
		if stmt != nil {
			_ = stmt.Close()
		}
	}
	statements = dbStatements{}
}

func logPoolStats() {
	ticker := time.NewTicker(10 * time.Second)
	go func() {
		defer ticker.Stop()
		var prevReader, prevWriter sql.DBStats
		for range ticker.C {
			reader, writer := readerDB.Stats(), writerDB.Stats()
			log.Printf("sqlite pools: reader open=%d in_use=%d idle=%d wait_count_10s=%d wait_duration_10s=%s; writer open=%d in_use=%d idle=%d wait_count_10s=%d wait_duration_10s=%s",
				reader.OpenConnections, reader.InUse, reader.Idle,
				reader.WaitCount-prevReader.WaitCount, (reader.WaitDuration - prevReader.WaitDuration).Round(time.Millisecond),
				writer.OpenConnections, writer.InUse, writer.Idle,
				writer.WaitCount-prevWriter.WaitCount, (writer.WaitDuration - prevWriter.WaitDuration).Round(time.Millisecond))
			prevReader, prevWriter = reader, writer
		}
	}()
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
	readerLimit, err := poolLimit("DB_READERS", 8)
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
	log.Printf("SQLite pools ready: readers=%d writers=%d", readerLimit, writerLimit)
	logPoolStats()
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
	s := &http.Server{Addr: host + ":" + port, Handler: http.HandlerFunc(handler), ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 75 * time.Second, MaxHeaderBytes: 16 << 10}
	log.Fatal(s.ListenAndServe())
}
