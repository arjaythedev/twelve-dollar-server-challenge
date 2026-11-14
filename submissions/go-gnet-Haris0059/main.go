package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"log"
	"net"
	"os"
	"runtime/debug"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

// SQLite builds the response JSON itself: one row per request.
const postObj = `json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))`

const (
	feedSQL   = `SELECT '{"posts":[' || coalesce(group_concat(j, ','), '') || ']}' FROM (SELECT ` + postObj + ` AS j FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`
	postSQL   = `SELECT '{"post":' || ` + postObj + ` || '}' FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`
	existsSQL = "SELECT 1 FROM posts WHERE id = ?"
	insertSQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
	likeSQL   = "INSERT INTO likes (user_id, post_id) SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?) ON CONFLICT DO NOTHING"
	pragmas   = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY;"
)

var (
	start     = time.Now()
	jwtSecret []byte
)

func main() {
	debug.SetGCPercent(400)
	debug.SetMemoryLimit(1200 << 20)
	jwtSecret = []byte(os.Getenv("JWT_SECRET"))
	dbOpen(os.Getenv("SQLITE_PATH"))

	host, port := os.Getenv("HOST"), os.Getenv("PORT")
	if host == "" {
		host = "0.0.0.0"
	}
	if port == "" {
		port = "3000"
	}
	log.Fatal(listen(net.JoinHostPort(host, port)))
}

func fail(out []byte, status int, msg string) (int, []byte) {
	return status, append(append(append(out, `{"error":"`...), msg...), `"}`...)
}

// serve handles one request and appends the JSON response body to out.
func serve(method, path, auth string, body, out []byte) (int, []byte) {
	get, post := method == "GET", method == "POST"
	switch {
	case path == "/feed" && get:
		return feed(out)
	case path == "/health" && get:
		return health(out)
	case path == "/posts" && post:
		return createPost(auth, body, out)
	case strings.HasPrefix(path, "/posts/"):
		rest := path[len("/posts/"):]
		if post && strings.HasSuffix(rest, "/like") {
			return like(auth, strings.TrimSuffix(rest, "/like"), out)
		} else if get {
			return getPost(rest, out)
		}
	}
	return fail(out, 404, "not found")
}

func health(out []byte) (int, []byte) {
	if err := dbPing(); err != nil {
		b, _ := json.Marshal(map[string]string{"status": "degraded", "db": "unreachable", "error": err.Error()})
		return 503, append(out, b...)
	}
	out = append(out, `{"status":"ok","db":"ok","uptime_s":`...)
	out = strconv.AppendInt(out, int64(time.Since(start).Seconds()), 10)
	return 200, append(out, '}')
}

func feed(out []byte) (int, []byte) {
	b, err := dbFeed(out)
	if err != nil {
		return fail(out, 500, "internal server error")
	}
	return 200, b
}

func getPost(idStr string, out []byte) (int, []byte) {
	id, ok := parseID(idStr)
	if !ok {
		return fail(out, 400, "invalid post id")
	}
	b, found, err := dbPost(out, id)
	switch {
	case err != nil:
		return fail(out, 500, "internal server error")
	case !found:
		return fail(out, 404, "post not found")
	}
	return 200, b
}

func createPost(auth string, reqBody, out []byte) (int, []byte) {
	uid, username, status, msg := authenticate(auth)
	if msg != "" {
		return fail(out, status, msg)
	}
	var in map[string]any
	if json.Unmarshal(reqBody, &in) != nil {
		return fail(out, 400, "malformed JSON body")
	}
	body, _ := in["body"].(string)
	body = strings.TrimSpace(body)
	if body == "" {
		return fail(out, 400, "body is required")
	}
	if utf8.RuneCountInString(body) > 500 {
		return fail(out, 400, "body must be at most 500 characters")
	}
	id, created, err := dbInsert(uid, body)
	if err != nil {
		log.Println("insert:", err)
		return fail(out, 500, "internal server error")
	}
	out = appendPost(append(out, `{"post":`...), id, []byte(body), []byte(created), []byte(username), 0)
	return 201, append(out, '}')
}

func like(auth, idStr string, out []byte) (int, []byte) {
	uid, _, status, msg := authenticate(auth)
	if msg != "" {
		return fail(out, status, msg)
	}
	id, ok := parseID(idStr)
	if !ok {
		return fail(out, 400, "invalid post id")
	}
	inserted, exists, err := dbLike(uid, id)
	switch {
	case err != nil:
		return fail(out, 500, "internal server error")
	case !exists:
		return fail(out, 404, "post not found")
	}
	code := 200
	out = append(out, `{"liked":true,"already_liked":`...)
	if inserted {
		code = 201
		out = append(out, `false`...)
	} else {
		out = append(out, `true`...)
	}
	out = strconv.AppendInt(append(out, `,"post_id":`...), id, 10)
	return code, append(out, '}')
}

func parseID(s string) (int64, bool) {
	if len(s) == 0 || len(s) > 18 {
		return 0, false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
	}
	id, _ := strconv.ParseInt(s, 10, 64)
	return id, id > 0
}

func authenticate(h string) (uid int64, username string, status int, msg string) {
	if !strings.HasPrefix(h, "Bearer ") {
		return 0, "", 401, "missing bearer token"
	}
	uid, username, ok, payloadOK := verifyJWT(h[len("Bearer "):], time.Now().Unix())
	switch {
	case !ok:
		return 0, "", 401, "invalid or expired token"
	case !payloadOK:
		return 0, "", 401, "invalid token payload"
	}
	return uid, username, 0, ""
}

// verifyJWT checks HS256 signature and exp on every call (no caching, rule 5).
func verifyJWT(tok string, now int64) (uid int64, username string, ok, payloadOK bool) {
	parts := strings.Split(tok, ".")
	if len(parts) != 3 {
		return
	}
	enc := base64.RawURLEncoding
	hdr, err1 := enc.DecodeString(parts[0])
	pl, err2 := enc.DecodeString(parts[1])
	sig, err3 := enc.DecodeString(parts[2])
	if err1 != nil || err2 != nil || err3 != nil {
		return
	}
	var h struct{ Alg string }
	if json.Unmarshal(hdr, &h) != nil || h.Alg != "HS256" {
		return
	}
	m := hmac.New(sha256.New, jwtSecret)
	m.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(sig, m.Sum(nil)) {
		return
	}
	var p struct {
		Sub      any
		Username any
		Exp      *float64
		Nbf      *float64
	}
	if json.Unmarshal(pl, &p) != nil {
		return
	}
	if (p.Exp != nil && float64(now) >= *p.Exp) || (p.Nbf != nil && float64(now) < *p.Nbf) {
		return
	}
	ok = true
	sub, _ := p.Sub.(string)
	name, isStr := p.Username.(string)
	uid, idOK := parseID(sub)
	if !idOK || !isStr {
		return
	}
	return uid, name, true, true
}

func appendPost(b []byte, id int64, body, created, author []byte, likes int64) []byte {
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, `,"body":`...)
	b = appendString(b, body)
	b = append(b, `,"created_at":`...)
	b = appendString(b, created)
	b = append(b, `,"author":`...)
	b = appendString(b, author)
	b = append(b, `,"like_count":`...)
	b = strconv.AppendInt(b, likes, 10)
	return append(b, '}')
}

const hex = "0123456789abcdef"

func appendString(b, s []byte) []byte {
	b = append(b, '"')
	for _, c := range s {
		switch {
		case c == '"' || c == '\\':
			b = append(b, '\\', c)
		case c == '\n':
			b = append(b, '\\', 'n')
		case c == '\r':
			b = append(b, '\\', 'r')
		case c == '\t':
			b = append(b, '\\', 't')
		case c < 0x20:
			b = append(b, '\\', 'u', '0', '0', hex[c>>4], hex[c&15])
		default:
			b = append(b, c)
		}
	}
	return append(b, '"')
}
