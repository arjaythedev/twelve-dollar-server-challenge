// A minimal feed API: net/http + database/sql + mattn/go-sqlite3.
package main

import (
	"bytes"
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

type app struct {
	db      *sql.DB
	secret  []byte
	started time.Time
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
		"?_busy_timeout=5000&_journal_mode=WAL&_synchronous=NORMAL"
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
		a.feed(w, r)
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
		a.getPost(w, r, rest)
	}
}

func (a *app) health(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	var one int
	if err := a.db.QueryRowContext(ctx, "SELECT 1").Scan(&one); err != nil {
		writeJSON(w, http.StatusServiceUnavailable, struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Error  string `json:"error"`
		}{"degraded", "unreachable", err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Status  string `json:"status"`
		DB      string `json:"db"`
		UptimeS int64  `json:"uptime_s"`
	}{"ok", "ok", int64(time.Since(a.started).Seconds())})
}

func (a *app) feed(w http.ResponseWriter, r *http.Request) {
	rows, err := a.db.QueryContext(r.Context(), `
		SELECT p.id, p.body, p.created_at, u.username,
		       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
		  FROM posts p JOIN users u ON u.id = p.user_id
		 ORDER BY p.created_at DESC, p.id DESC
		 LIMIT 20`)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	defer rows.Close()

	posts := make([]post, 0, 20)
	for rows.Next() {
		var p post
		if err := rows.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount); err != nil {
			writeError(w, http.StatusInternalServerError, "internal server error")
			return
		}
		posts = append(posts, p)
	}
	if err := rows.Err(); err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Posts []post `json:"posts"`
	}{posts})
}

func (a *app) getPost(w http.ResponseWriter, r *http.Request, idStr string) {
	id, ok := parsePostID(idStr)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	p, err := queryPost(r.Context(), a.db, id)
	if err == sql.ErrNoRows {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Post post `json:"post"`
	}{p})
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

	var p post
	p.Body, p.Author = body, username
	err = a.db.QueryRowContext(r.Context(),
		"INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
		userID, body).Scan(&p.ID, &p.CreatedAt)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	writeJSON(w, http.StatusCreated, struct {
		Post post `json:"post"`
	}{p})
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
	err := a.db.QueryRowContext(r.Context(), "SELECT 1 FROM posts WHERE id = ?", id).Scan(&one)
	if err == sql.ErrNoRows {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}

	res, err := a.db.ExecContext(r.Context(),
		"INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING",
		userID, id)
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
	writeJSON(w, status, struct {
		Liked        bool  `json:"liked"`
		AlreadyLiked bool  `json:"already_liked"`
		PostID       int64 `json:"post_id"`
	}{true, already, id})
}

func queryPost(ctx context.Context, db *sql.DB, id int64) (post, error) {
	var p post
	err := db.QueryRowContext(ctx, `
		SELECT p.id, p.body, p.created_at, u.username,
		       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
		  FROM posts p JOIN users u ON u.id = p.user_id
		 WHERE p.id = ?`, id).
		Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount)
	return p, err
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

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, struct {
		Error string `json:"error"`
	}{msg})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		http.Error(w, `{"error":"internal server error"}`, http.StatusInternalServerError)
		return
	}
	h := w.Header()
	h.Set("Content-Type", "application/json")
	h.Set("Content-Length", strconv.Itoa(buf.Len()))
	w.WriteHeader(status)
	w.Write(buf.Bytes())
}
