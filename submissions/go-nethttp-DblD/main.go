// Baseline entry: net/http + mattn/go-sqlite3, standard library everywhere else.
//
// Shape: a pool of read connections and one write connection (SQLite allows one writer at
// a time; a single connection makes that explicit instead of retrying on SQLITE_BUSY).
// Every query is a prepared statement. No caches of any kind (rule 5).
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	_ "github.com/mattn/go-sqlite3"
)

type post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

const postCols = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id `

type server struct {
	started time.Time
	secret  []byte

	health, feed, onePost, postExists *sql.Stmt // read pool
	insertPost, insertLike            *sql.Stmt // single writer
}

func main() {
	path := mustEnv("SQLITE_PATH")
	s := &server{started: time.Now(), secret: []byte(mustEnv("JWT_SECRET"))}

	// synchronous=NORMAL in WAL mode is the weakest setting rule 6 allows.
	dsn := fmt.Sprintf("file:%s?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000", path)
	rd := mustOpen(dsn, 4)
	wr := mustOpen(dsn+"&_txlock=immediate", 1)

	s.health = mustPrep(rd, `SELECT 1`)
	s.feed = mustPrep(rd, postCols+`ORDER BY p.created_at DESC, p.id DESC LIMIT 20`)
	s.onePost = mustPrep(rd, postCols+`WHERE p.id = ?`)
	s.postExists = mustPrep(rd, `SELECT 1 FROM posts WHERE id = ?`)
	s.insertPost = mustPrep(wr, `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`)
	s.insertLike = mustPrep(wr, `INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING`)

	addr := net.JoinHostPort(mustEnv("HOST"), mustEnv("PORT"))
	srv := &http.Server{
		Addr:              addr,
		Handler:           s,
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       120 * time.Second, // spec: keep idle sockets >= 65 s for Nginx
	}
	log.Printf("listening on %s", addr)
	log.Fatal(srv.ListenAndServe())
}

func (s *server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	p := r.URL.Path
	switch {
	case p == "/health":
		s.handleHealth(w)
	case p == "/feed":
		s.handleFeed(w)
	case p == "/posts" && r.Method == http.MethodPost:
		s.handleCreate(w, r)
	case strings.HasPrefix(p, "/posts/"):
		rest := p[len("/posts/"):]
		if id, ok := strings.CutSuffix(rest, "/like"); ok && r.Method == http.MethodPost {
			s.handleLike(w, r, id)
		} else if !strings.Contains(rest, "/") {
			s.handleGet(w, rest)
		} else {
			fail(w, 404, "not found")
		}
	default:
		fail(w, 404, "not found")
	}
}

func (s *server) handleHealth(w http.ResponseWriter) {
	var one int
	if err := s.health.QueryRow().Scan(&one); err != nil {
		send(w, 503, map[string]string{"status": "degraded", "db": "unreachable", "error": err.Error()})
		return
	}
	// Literal so the key order is fixed (a map would sort keys).
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(200)
	fmt.Fprintf(w, `{"status":"ok","db":"ok","uptime_s":%d}`, int64(time.Since(s.started).Seconds()))
}

func (s *server) handleFeed(w http.ResponseWriter) {
	rows, err := s.feed.Query()
	if err != nil {
		internal(w, err)
		return
	}
	defer rows.Close()
	posts := make([]post, 0, 20)
	for rows.Next() {
		var p post
		if err := rows.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount); err != nil {
			internal(w, err)
			return
		}
		posts = append(posts, p)
	}
	if err := rows.Err(); err != nil {
		internal(w, err)
		return
	}
	send(w, 200, struct {
		Posts []post `json:"posts"`
	}{posts})
}

func (s *server) handleGet(w http.ResponseWriter, rawID string) {
	id, ok := parseID(rawID)
	if !ok {
		fail(w, 400, "invalid post id")
		return
	}
	var p post
	err := s.onePost.QueryRow(id).Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		fail(w, 404, "post not found")
	case err != nil:
		internal(w, err)
	default:
		send(w, 200, struct {
			Post post `json:"post"`
		}{p})
	}
}

func (s *server) handleCreate(w http.ResponseWriter, r *http.Request) {
	uid, uname, ok := s.auth(w, r)
	if !ok {
		return
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, 64<<10))
	if err != nil {
		fail(w, 400, "malformed JSON body")
		return
	}
	var in map[string]any
	if err := json.Unmarshal(raw, &in); err != nil {
		fail(w, 400, "malformed JSON body")
		return
	}
	str, _ := in["body"].(string)
	body := strings.TrimSpace(str)
	switch {
	case body == "":
		fail(w, 400, "body is required")
		return
	case utf8.RuneCountInString(body) > 500: // characters, as SQLite's length() counts them
		fail(w, 400, "body must be at most 500 characters")
		return
	}
	p := post{Body: body, Author: uname}
	if err := s.insertPost.QueryRow(uid, body).Scan(&p.ID, &p.CreatedAt); err != nil {
		internal(w, err)
		return
	}
	send(w, 201, struct {
		Post post `json:"post"`
	}{p})
}

func (s *server) handleLike(w http.ResponseWriter, r *http.Request, rawID string) {
	uid, _, ok := s.auth(w, r) // auth before id, per spec
	if !ok {
		return
	}
	id, ok := parseID(rawID)
	if !ok {
		fail(w, 400, "invalid post id")
		return
	}
	var one int
	if err := s.postExists.QueryRow(id).Scan(&one); errors.Is(err, sql.ErrNoRows) {
		fail(w, 404, "post not found")
		return
	} else if err != nil {
		internal(w, err)
		return
	}
	res, err := s.insertLike.Exec(uid, id)
	if err != nil {
		internal(w, err)
		return
	}
	n, _ := res.RowsAffected()
	status := 200
	if n == 1 {
		status = 201
	}
	send(w, status, struct {
		Liked        bool  `json:"liked"`
		AlreadyLiked bool  `json:"already_liked"`
		PostID       int64 `json:"post_id"`
	}{true, n == 0, id})
}

// auth verifies an HS256 bearer token and returns (user id, username). On failure it has
// already written the 401.
func (s *server) auth(w http.ResponseWriter, r *http.Request) (int64, string, bool) {
	tok, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
	if !ok {
		fail(w, 401, "missing bearer token")
		return 0, "", false
	}
	claims, ok := s.verify(tok)
	if !ok {
		fail(w, 401, "invalid or expired token")
		return 0, "", false
	}
	sub, _ := claims["sub"].(string)
	name, nameOK := claims["username"].(string)
	uid, idOK := parseID(sub)
	if !idOK || !nameOK {
		fail(w, 401, "invalid token payload")
		return 0, "", false
	}
	return uid, name, true
}

func (s *server) verify(tok string) (map[string]any, bool) {
	parts := strings.Split(tok, ".")
	if len(parts) != 3 {
		return nil, false
	}
	var hdr struct {
		Alg string `json:"alg"`
	}
	if !decodeSeg(parts[0], &hdr) || hdr.Alg != "HS256" {
		return nil, false
	}
	sig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return nil, false
	}
	mac := hmac.New(sha256.New, s.secret)
	mac.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(sig, mac.Sum(nil)) {
		return nil, false
	}
	var claims map[string]any
	if !decodeSeg(parts[1], &claims) {
		return nil, false
	}
	exp, ok := claims["exp"].(float64)
	if !ok || float64(time.Now().Unix()) >= exp {
		return nil, false
	}
	return claims, true
}

func decodeSeg(seg string, v any) bool {
	b, err := base64.RawURLEncoding.DecodeString(seg)
	return err == nil && json.Unmarshal(b, v) == nil
}

// parseID accepts only a positive base-10 integer: no sign, no fraction, no spaces.
func parseID(s string) (int64, bool) {
	if s == "" || s[0] < '1' || s[0] > '9' {
		return 0, false
	}
	n, err := strconv.ParseInt(s, 10, 64)
	return n, err == nil
}

func send(w http.ResponseWriter, status int, v any) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		internal(w, err)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(buf.Bytes())
}

func fail(w http.ResponseWriter, status int, msg string) {
	send(w, status, map[string]string{"error": msg})
}

func internal(w http.ResponseWriter, err error) {
	log.Printf("internal: %v", err)
	fail(w, 500, "internal server error")
}

func mustEnv(k string) string {
	v := os.Getenv(k)
	if v == "" {
		log.Fatalf("%s is not set", k)
	}
	return v
}

func mustOpen(dsn string, conns int) *sql.DB {
	db, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	db.SetMaxOpenConns(conns)
	db.SetMaxIdleConns(conns)
	db.SetConnMaxLifetime(0)
	return db
}

func mustPrep(db *sql.DB, q string) *sql.Stmt {
	st, err := db.Prepare(q)
	if err != nil {
		log.Fatalf("prepare %q: %v", q, err)
	}
	return st
}
