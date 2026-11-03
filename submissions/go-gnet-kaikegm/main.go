package main

import (
	"bytes"
	"encoding/json"
	"log"
	"net"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
	"unsafe"
)

var start = time.Now()

func main() {
	// Set memory limit and GC tuning for the 1 vCPU, 2 GB VM target
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(400)
	debug.SetMemoryLimit(1200 << 20)

	secret := os.Getenv("JWT_SECRET")
	if secret == "" {
		secret = "twelve-dollar-challenge"
	}
	setJWTSecret([]byte(secret))

	dbPath := os.Getenv("SQLITE_PATH")
	if dbPath == "" {
		log.Fatal("SQLITE_PATH environment variable is required")
	}
	dbOpen(dbPath)

	host, port := os.Getenv("HOST"), os.Getenv("PORT")
	if host == "" {
		host = "0.0.0.0"
	}
	if port == "" {
		port = "3000"
	}

	addr := net.JoinHostPort(host, port)
	log.Printf("Listening on %s...", addr)
	log.Fatal(listen(addr))
}

func fail(out []byte, status int, msg string) (int, []byte) {
	return status, append(append(append(out, `{"error":"`...), msg...), `"}`...)
}

func serve(method, path, auth, body, out []byte) (int, []byte) {
	if len(path) < 2 || path[0] != '/' {
		return fail(out, 404, "not found")
	}

	isGet := len(method) == 3 && method[0] == 'G' && method[1] == 'E' && method[2] == 'T'
	isPost := len(method) == 4 && method[0] == 'P' && method[1] == 'O' && method[2] == 'S' && method[3] == 'T'

	switch path[1] {
	case 'f':
		if isGet && bytes.Equal(path, []byte("/feed")) {
			return feed(out)
		}
	case 'h':
		if isGet && bytes.Equal(path, []byte("/health")) {
			return health(out)
		}
	case 'p':
		if isPost && bytes.Equal(path, []byte("/posts")) {
			return createPost(auth, body, out)
		} else if len(path) > 7 && bytes.HasPrefix(path, []byte("/posts/")) {
			rest := path[7:]
			if isPost && bytes.HasSuffix(rest, []byte("/like")) {
				return like(auth, rest[:len(rest)-5], out)
			} else if isGet {
				return getPost(rest, out)
			}
		}
	}

	return fail(out, 404, "not found")
}

func health(out []byte) (int, []byte) {
	if err := dbPing(); err != nil {
		b, _ := json.Marshal(map[string]string{
			"status": "degraded",
			"db":     "unreachable",
			"error":  err.Error(),
		})
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

func getPost(idBytes, out []byte) (int, []byte) {
	id, ok := parseID(idBytes)
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

func createPost(auth, reqBody, out []byte) (int, []byte) {
	// 1. Auth check first
	uid, username, status, msg := authenticate(auth)
	if msg != "" {
		return fail(out, status, msg)
	}

	// 2. Single-pass JSON body unmarshaling (only runs json.Valid on error)
	var in struct {
		Body *string `json:"body"`
	}
	if err := json.Unmarshal(reqBody, &in); err != nil {
		if !json.Valid(reqBody) {
			return fail(out, 400, "malformed JSON body")
		}
		return fail(out, 400, "body is required")
	}
	if in.Body == nil {
		return fail(out, 400, "body is required")
	}
	trimmed := strings.TrimSpace(*in.Body)
	if trimmed == "" {
		return fail(out, 400, "body is required")
	}
	if utf8.RuneCountInString(trimmed) > 500 {
		return fail(out, 400, "body must be at most 500 characters")
	}

	// 3. Database insert
	id, created, err := dbInsert(uid, trimmed)
	if err != nil {
		return fail(out, 500, "internal server error")
	}

	trimmedBytes := unsafe.Slice(unsafe.StringData(trimmed), len(trimmed))
	out = appendPost(append(out, `{"post":`...), id, trimmedBytes, created, username, 0)
	return 201, append(out, '}')
}

func like(auth, idBytes, out []byte) (int, []byte) {
	// 1. Auth check first (auth before id per spec!)
	uid, _, status, msg := authenticate(auth)
	if msg != "" {
		return fail(out, status, msg)
	}

	// 2. Validate post id
	id, ok := parseID(idBytes)
	if !ok {
		return fail(out, 400, "invalid post id")
	}

	// 3. Database like
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

func parseID(b []byte) (int64, bool) {
	if len(b) == 0 || len(b) > 18 {
		return 0, false
	}
	var id int64
	for i := 0; i < len(b); i++ {
		c := b[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		id = id*10 + int64(c-'0')
	}
	return id, id > 0
}

func appendPost(b []byte, id int64, body, created, author []byte, likes int64) []byte {
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, `,"body":`...)
	b = appendJSONString(b, body)
	b = append(b, `,"created_at":"`...)
	b = append(b, created...)
	b = append(b, `","author":`...)
	b = appendJSONString(b, author)
	b = append(b, `,"like_count":`...)
	b = strconv.AppendInt(b, likes, 10)
	return append(b, '}')
}

const hexDigits = "0123456789abcdef"

func appendJSONString(b, s []byte) []byte {
	b = append(b, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < 0x20 || c == '"' || c == '\\' {
			if i > start {
				b = append(b, s[start:i]...)
			}
			switch c {
			case '"', '\\':
				b = append(b, '\\', c)
			case '\n':
				b = append(b, '\\', 'n')
			case '\r':
				b = append(b, '\\', 'r')
			case '\t':
				b = append(b, '\\', 't')
			case '\b':
				b = append(b, '\\', 'b')
			case '\f':
				b = append(b, '\\', 'f')
			default:
				b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&15])
			}
			start = i + 1
		}
	}
	if len(s) > start {
		b = append(b, s[start:]...)
	}
	return append(b, '"')
}
