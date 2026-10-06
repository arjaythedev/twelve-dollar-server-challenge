package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	sqlite "github.com/mattn/go-sqlite3"
)

type Post struct {
	ID      int64  `json:"id"`
	Body    string `json:"body"`
	Created string `json:"created_at"`
	Author  string `json:"author"`
	Likes   int64  `json:"like_count"`
}
type reply struct {
	status int
	data   []byte
}
type job struct {
	method, path, auth string
	body               []byte
	done               chan reply
}
type app struct {
	conn                *sql.Conn
	statements          map[string]*sql.Stmt
	secret              []byte
	started             time.Time
	transaction, failed bool
}

var ctx = context.Background()

func response(status int, value any) reply {
	data, err := json.Marshal(value)
	if err != nil {
		return failure(500, "internal server error")
	}
	return reply{status, data}
}
func failure(status int, message string) reply {
	return response(status, struct {
		Error string `json:"error"`
	}{message})
}
func positive(s string) int64 {
	if s == "" {
		return 0
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0
		}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n <= 0 || n > 9007199254740991 {
		return 0
	}
	return n
}
func space(c rune) bool {
	return c >= 9 && c <= 13 || c == 32 || c == 0xa0 || c == 0x1680 ||
		c >= 0x2000 && c <= 0x200a || c == 0x2028 || c == 0x2029 ||
		c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff
}
func (a *app) authenticate(auth string) (int64, string, string) {
	if !strings.HasPrefix(auth, "Bearer ") {
		return 0, "", "missing bearer token"
	}
	invalid := "invalid or expired token"
	token := strings.TrimPrefix(auth, "Bearer ")
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return 0, "", invalid
	}
	decoded := make([][]byte, 3)
	for i, part := range parts {
		for _, c := range part {
			if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' || c == '_') {
				return 0, "", invalid
			}
		}
		var err error
		decoded[i], err = base64.RawURLEncoding.DecodeString(part)
		if err != nil || len(decoded[i]) == 0 {
			return 0, "", invalid
		}
	}
	mac := hmac.New(sha256.New, a.secret)
	mac.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(mac.Sum(nil), decoded[2]) {
		return 0, "", invalid
	}
	var head, claims map[string]any
	if !utf8.Valid(decoded[0]) || !utf8.Valid(decoded[1]) || json.Unmarshal(decoded[0], &head) != nil || json.Unmarshal(decoded[1], &claims) != nil || head["alg"] != "HS256" {
		return 0, "", invalid
	}
	now := float64(time.Now().UnixNano()) / 1e9
	exp, ok := claims["exp"].(float64)
	if !ok || exp <= now {
		return 0, "", invalid
	}
	if value, present := claims["nbf"]; present {
		nbf, ok := value.(float64)
		if !ok || nbf > now {
			return 0, "", invalid
		}
	}
	sub, subOK := claims["sub"].(string)
	name, nameOK := claims["username"].(string)
	id := positive(sub)
	if !subOK || !nameOK || id == 0 {
		return 0, "", "invalid token payload"
	}
	return id, name, ""
}
func (a *app) begin() error {
	if a.transaction {
		return nil
	}
	_, err := a.conn.ExecContext(ctx, "BEGIN IMMEDIATE")
	a.transaction = err == nil
	return err
}
func (a *app) handle(j job) reply {
	internal := func() reply {
		// Some SQLite errors roll back the entire transaction automatically.
		if a.transaction {
			err := a.conn.Raw(func(c any) error { a.failed = c.(*sqlite.SQLiteConn).AutoCommit(); return nil })
			if err != nil {
				a.failed = true
			}
		}
		return failure(500, "internal server error")
	}
	if a.failed {
		return internal()
	}
	if j.method == "GET" && j.path == "/health" {
		var one int
		if err := a.statements["health"].QueryRow().Scan(&one); err != nil {
			return response(503, struct {
				Status string `json:"status"`
				DB     string `json:"db"`
				Error  string `json:"error"`
			}{"degraded", "unreachable", err.Error()})
		}
		return response(200, struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Uptime int64  `json:"uptime_s"`
		}{"ok", "ok", int64(time.Since(a.started).Seconds())})
	}
	create := j.method == "POST" && j.path == "/posts"
	like := j.method == "POST" && strings.HasSuffix(j.path, "/like")
	idText := ""
	if strings.HasPrefix(j.path, "/posts/") && (j.method == "GET" || like) {
		idText = strings.TrimPrefix(j.path, "/posts/")
		if like {
			idText = strings.TrimSuffix(idText, "/like")
		}
		if strings.Contains(idText, "/") {
			idText = ""
		}
	}
	feed := j.method == "GET" && j.path == "/feed"
	if !create && !feed && idText == "" {
		return failure(404, "not found")
	}
	var user int64
	var name, message string
	if j.method == "POST" {
		user, name, message = a.authenticate(j.auth)
		if message != "" {
			return failure(401, message)
		}
	}
	id := positive(idText)
	if !create && !feed && id == 0 {
		return failure(400, "invalid post id")
	}
	if create {
		var value any
		if !utf8.Valid(j.body) || json.Unmarshal(j.body, &value) != nil {
			return failure(400, "malformed JSON body")
		}
		object, _ := value.(map[string]any)
		body, _ := object["body"].(string)
		body = strings.TrimFunc(body, space)
		if body == "" {
			return failure(400, "body is required")
		}
		if utf8.RuneCountInString(body) > 500 {
			return failure(400, "body must be at most 500 characters")
		}
		if a.begin() != nil {
			return internal()
		}
		p := Post{Body: body, Author: name}
		if a.statements["create"].QueryRow(user, body).Scan(&p.ID, &p.Created) != nil {
			return internal()
		}
		return response(201, struct {
			Post Post `json:"post"`
		}{p})
	}
	if like {
		if a.begin() != nil {
			return internal()
		}
		result, err := a.statements["like"].Exec(user, id)
		if err != nil {
			return internal()
		}
		changed, err := result.RowsAffected()
		if err != nil {
			return internal()
		}
		if changed == 0 {
			var one int
			err = a.statements["exists"].QueryRow(id).Scan(&one)
			if err == sql.ErrNoRows {
				return failure(404, "post not found")
			}
			if err != nil {
				return internal()
			}
		}
		status := 200
		if changed != 0 {
			status = 201
		}
		return response(status, struct {
			Liked   bool  `json:"liked"`
			Already bool  `json:"already_liked"`
			ID      int64 `json:"post_id"`
		}{true, changed == 0, id})
	}
	statement, params := a.statements["post"], []any{id}
	if feed {
		statement, params = a.statements["feed"], nil
	}
	rows, err := statement.Query(params...)
	if err != nil {
		return internal()
	}
	defer rows.Close()
	posts := make([]Post, 0, 20)
	for rows.Next() {
		var p Post
		if rows.Scan(&p.ID, &p.Body, &p.Created, &p.Author, &p.Likes) != nil {
			return internal()
		}
		posts = append(posts, p)
	}
	if rows.Err() != nil {
		return internal()
	}
	if feed {
		return response(200, struct {
			Posts []Post `json:"posts"`
		}{posts})
	}
	if len(posts) == 0 {
		return failure(404, "post not found")
	}
	return response(200, struct {
		Post Post `json:"post"`
	}{posts[0]})
}
func (a *app) work(queue <-chan job) {
	jobs, replies := make([]job, 0, 256), make([]reply, 0, 256)
	for first := range queue {
		jobs, replies = append(jobs[:0], first), replies[:0]
		runtime.Gosched() // Let ready HTTP goroutines enqueue; no batching timer.
		for i := 0; i < len(jobs); i++ {
			replies = append(replies, a.handle(jobs[i]))
			if len(jobs) < 256 {
				select {
				case j := <-queue:
					jobs = append(jobs, j)
				default:
				}
			}
		}
		if a.transaction {
			if !a.failed {
				_, err := a.conn.ExecContext(ctx, "COMMIT")
				a.failed = err != nil
			}
			if a.failed {
				_, _ = a.conn.ExecContext(ctx, "ROLLBACK")
			}
		}
		for i, j := range jobs {
			if a.failed {
				replies[i] = failure(500, "internal server error")
			}
			j.done <- replies[i]
			jobs[i] = job{}
			replies[i] = reply{}
		}
		a.transaction, a.failed = false, false
	}
}
func main() {
	runtime.GOMAXPROCS(1)
	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		log.Fatal("SQLITE_PATH and JWT_SECRET are required")
	}
	dsn := (&url.URL{Scheme: "file", Path: path}).String() + "?mode=rw&_journal_mode=WAL&_synchronous=NORMAL&_foreign_keys=on&_locking_mode=EXCLUSIVE&_busy_timeout=0"
	db, err := sql.Open("sqlite3", dsn)
	if err != nil {
		log.Fatal(err)
	}
	db.SetMaxOpenConns(1)
	conn, err := db.Conn(ctx)
	if err != nil {
		log.Fatal(err)
	}
	a := app{conn: conn, statements: make(map[string]*sql.Stmt), secret: []byte(secret), started: time.Now()}
	if _, err = conn.ExecContext(ctx, "PRAGMA cache_size=-8192; PRAGMA mmap_size=268435456;"); err != nil {
		log.Fatal(err)
	}
	const read = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id"
	for name, query := range map[string]string{
		"feed": read + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20", "post": read + " WHERE p.id=?",
		"create": "INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at",
		"like":   "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING",
		"exists": "SELECT 1 FROM posts WHERE id=?", "health": "SELECT 1",
	} {
		a.statements[name], err = conn.PrepareContext(ctx, query)
		if err != nil {
			log.Fatal(err)
		}
	}
	queue := make(chan job, 1024)
	go a.work(queue)
	server := &http.Server{Addr: net.JoinHostPort(env("HOST", "127.0.0.1"), env("PORT", "3000")),
		ReadHeaderTimeout: 75 * time.Second, ReadTimeout: 75 * time.Second, WriteTimeout: 75 * time.Second, IdleTimeout: 75 * time.Second, MaxHeaderBytes: 16384}
	server.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 16384))
		if err != nil {
			result := failure(413, "request body too large")
			w.WriteHeader(result.status)
			_, _ = w.Write(result.data)
			return
		}
		j := job{r.Method, r.URL.Path, r.Header.Get("Authorization"), body, make(chan reply, 1)}
		select {
		case queue <- j:
		case <-r.Context().Done():
			return
		}
		select {
		case result := <-j.done:
			w.Header().Set("Content-Length", strconv.Itoa(len(result.data)))
			w.WriteHeader(result.status)
			_, _ = w.Write(result.data)
		case <-r.Context().Done():
		}
	})
	fmt.Fprintln(os.Stderr, "listening on", server.Addr)
	log.Fatal(server.ListenAndServe())
}
func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}
