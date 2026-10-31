// A minimal feed API: net/http + database/sql + mattn/go-sqlite3.
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

type app struct {
	db         *sql.DB
	secret     []byte
	started    time.Time
	feed       *sql.Stmt
	getPost    *sql.Stmt
	insertPost *sql.Stmt
	postExists *sql.Stmt
	insertLike *sql.Stmt
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
	db, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	db.SetMaxOpenConns(8)
	db.SetMaxIdleConns(8)

	a := &app{
		db:      db,
		secret:  []byte(os.Getenv("JWT_SECRET")),
		started: time.Now(),
	}
	a.feed, err = db.Prepare(`
		SELECT p.id, p.body, p.created_at, u.username,
		       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
		  FROM posts p JOIN users u ON u.id = p.user_id
		 ORDER BY p.created_at DESC, p.id DESC
		 LIMIT 20`)
	if err != nil {
		log.Fatal(err)
	}
	a.getPost, err = db.Prepare(`
		SELECT p.id, p.body, p.created_at, u.username,
		       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
		  FROM posts p JOIN users u ON u.id = p.user_id
		 WHERE p.id = ?`)
	if err != nil {
		log.Fatal(err)
	}
	a.insertPost, err = db.Prepare(
		"INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
	if err != nil {
		log.Fatal(err)
	}
	a.postExists, err = db.Prepare("SELECT 1 FROM posts WHERE id = ?")
	if err != nil {
		log.Fatal(err)
	}
	a.insertLike, err = db.Prepare(
		"INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING")
	if err != nil {
		log.Fatal(err)
	}

	srv := &http.Server{
		Addr:              host + ":" + port,
		Handler:           a,
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Fatal(srv.ListenAndServe())
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
	if err := a.db.QueryRowContext(ctx, "SELECT 1").Scan(&one); err != nil {
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
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		writeError(w, http.StatusBadRequest, "malformed JSON body")
		return
	}
	rawBody, ok := raw["body"]
	var body string
	if !ok || json.Unmarshal(rawBody, &body) != nil {
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

	p := post{Body: body, Author: username}
	err = a.insertPost.QueryRowContext(r.Context(), userID, body).Scan(&p.ID, &p.CreatedAt)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"post":`...)
	b = appendPost(b, p)
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

	var one int
	err := a.postExists.QueryRowContext(r.Context(), id).Scan(&one)
	if err == sql.ErrNoRows {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}

	res, err := a.insertLike.ExecContext(r.Context(), userID, id)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	n, err := res.RowsAffected()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	already := n == 0
	status := http.StatusCreated
	if already {
		status = http.StatusOK
	}
	bp := bufPool.Get().(*[]byte)
	b := append((*bp)[:0], `{"liked":true,"already_liked":`...)
	b = strconv.AppendBool(b, already)
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
	var claims map[string]json.RawMessage
	if json.Unmarshal(claimsJSON, &claims) != nil {
		return 0, "", invalid
	}
	var exp int64
	if json.Unmarshal(claims["exp"], &exp) != nil || exp <= time.Now().Unix() {
		return 0, "", invalid
	}

	var sub string
	if json.Unmarshal(claims["sub"], &sub) != nil {
		return 0, "", payload
	}
	userID, ok := parsePositiveInt(sub)
	if !ok {
		return 0, "", payload
	}
	var username string
	if json.Unmarshal(claims["username"], &username) != nil {
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
	for i := 0; i < len(s); i++ {
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
	h := w.Header()
	h.Set("Content-Type", "application/json")
	h.Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(status)
	w.Write(body)
}
