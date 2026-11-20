package main

import (
	"encoding/json"
	"errors"
	"log"
	"net"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/gofiber/fiber/v3"
	"github.com/gofiber/fiber/v3/middleware/recover"
)

var (
	errNotFound       = []byte(`{"error":"not found"}`)
	errInvalidPostID  = []byte(`{"error":"invalid post id"}`)
	errPostNotFound   = []byte(`{"error":"post not found"}`)
	errMissingBearer  = []byte(`{"error":"missing bearer token"}`)
	errInvalidToken   = []byte(`{"error":"invalid or expired token"}`)
	errInvalidPayload = []byte(`{"error":"invalid token payload"}`)
	errMalformedJSON  = []byte(`{"error":"malformed JSON body"}`)
	errBodyRequired   = []byte(`{"error":"body is required"}`)
	errBodyTooLong    = []byte(`{"error":"body must be at most 500 characters"}`)
	errInternal       = []byte(`{"error":"internal server error"}`)
)

const hexDigits = "0123456789abcdef"

var jsonBufPool = sync.Pool{
	New: func() any {
		b := make([]byte, 0, 8192)
		return &b
	},
}

func acquireJSONBuf() *[]byte {
	return jsonBufPool.Get().(*[]byte)
}

func releaseJSONBuf(b *[]byte) {
	if cap(*b) > 1<<20 {
		return
	}
	*b = (*b)[:0]
	jsonBufPool.Put(b)
}

func writeRaw(c fiber.Ctx, status int, body []byte) error {
	c.Set(fiber.HeaderContentType, fiber.MIMEApplicationJSON)
	c.Status(status)
	c.Response().SetBodyRaw(body)
	return nil
}

func writeJSON(c fiber.Ctx, status int, bufp *[]byte) error {
	c.Set(fiber.HeaderContentType, fiber.MIMEApplicationJSON)
	c.Status(status)
	resp := c.Response()
	resp.ResetBody()
	resp.AppendBody(*bufp)
	releaseJSONBuf(bufp)
	return nil
}

// writeJSONString sends a body that SQLite already encoded as JSON. SetBodyString
// copies it once into fasthttp's response buffer, so no extra []byte is allocated.
func writeJSONString(c fiber.Ctx, status int, body string) error {
	c.Set(fiber.HeaderContentType, fiber.MIMEApplicationJSON)
	c.Status(status)
	c.Response().SetBodyString(body)
	return nil
}

// appendJSONString appends a JSON string literal. Bytes >= 0x20 are emitted as
// UTF-8; only quote, backslash and control characters are escaped. Escaping
// <>&/ is optional per SPEC.md.
func appendJSONString(dst []byte, s string) []byte {
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
		case '\b':
			dst = append(dst, '\\', 'b')
		case '\f':
			dst = append(dst, '\\', 'f')
		default:
			dst = append(dst, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xF])
		}
		start = i + 1
	}
	dst = append(dst, s[start:]...)
	return append(dst, '"')
}

func appendPostJSON(dst []byte, p *Post) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, p.ID, 10)
	dst = append(dst, `,"body":`...)
	dst = appendJSONString(dst, p.Body)
	dst = append(dst, `,"created_at":`...)
	dst = appendJSONString(dst, p.CreatedAt)
	dst = append(dst, `,"author":`...)
	dst = appendJSONString(dst, p.Author)
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, p.LikeCount, 10)
	return append(dst, '}')
}

func writeLike(c fiber.Ctx, status int, already bool, id int64) error {
	buf := acquireJSONBuf()
	b := append(*buf, `{"liked":true,"already_liked":`...)
	if already {
		b = append(b, "true"...)
	} else {
		b = append(b, "false"...)
	}
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, '}')
	*buf = b
	return writeJSON(c, status, buf)
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func parsePostID(s string) (int64, bool) {
	id, err := strconv.ParseInt(s, 10, 64)
	return id, err == nil && id > 0
}

func main() {
	started := time.Now()

	// One SQLite connection is enough (and fastest) on the 1 vCPU scoring box:
	// every query shares one warm page cache instead of thrashing several. On a
	// larger box scale up, capped at 4. DB_POOL overrides for experiments.
	poolSize := runtime.GOMAXPROCS(0)
	if poolSize > 4 {
		poolSize = 4
	}
	if poolSize < 1 {
		poolSize = 1
	}
	if v := os.Getenv("DB_POOL"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 && n <= 32 {
			poolSize = n
		}
	}
	pool, err := newDBPool(envOr("SQLITE_PATH", "seed/feed.db"), poolSize)
	if err != nil {
		log.Fatalf("error opening the db: %v", err)
	}
	defer pool.close()

	verifier := newTokenVerifier([]byte(envOr("JWT_SECRET", "twelve-dollar-challenge")))

	app := fiber.New(fiber.Config{
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  75 * time.Second,
		ErrorHandler: func(c fiber.Ctx, err error) error {
			if errors.Is(err, fiber.ErrNotFound) {
				return writeRaw(c, fiber.StatusNotFound, errNotFound)
			}
			return writeRaw(c, fiber.StatusInternalServerError, errInternal)
		},
	})

	app.Use(recover.New())

	app.Get("/health", func(c fiber.Ctx) error {
		dc := pool.get()
		err := dc.ping()
		pool.put(dc)
		if err != nil {
			buf := acquireJSONBuf()
			b := append(*buf, `{"status":"degraded","db":"unreachable","error":`...)
			b = appendJSONString(b, err.Error())
			b = append(b, '}')
			*buf = b
			return writeJSON(c, fiber.StatusServiceUnavailable, buf)
		}

		buf := acquireJSONBuf()
		b := append(*buf, `{"status":"ok","db":"ok","uptime_s":`...)
		b = strconv.AppendInt(b, int64(time.Since(started)/time.Second), 10)
		b = append(b, '}')
		*buf = b
		return writeJSON(c, fiber.StatusOK, buf)
	})

	app.Get("/feed", func(c fiber.Ctx) error {
		dc := pool.get()
		body, err := dc.queryFeed()
		pool.put(dc)
		if err != nil {
			return writeRaw(c, fiber.StatusInternalServerError, errInternal)
		}
		return writeJSONString(c, fiber.StatusOK, body)
	})

	app.Get("/posts/:id", func(c fiber.Ctx) error {
		id, ok := parsePostID(c.Params("id"))
		if !ok {
			return writeRaw(c, fiber.StatusBadRequest, errInvalidPostID)
		}

		dc := pool.get()
		body, found, err := dc.queryPost(id)
		pool.put(dc)
		if err != nil {
			return writeRaw(c, fiber.StatusInternalServerError, errInternal)
		}
		if !found {
			return writeRaw(c, fiber.StatusNotFound, errPostNotFound)
		}
		return writeJSONString(c, fiber.StatusOK, body)
	})

	app.Post("/posts", func(c fiber.Ctx) error {
		user, ok := verifier.requireUser(c)
		if !ok {
			return nil
		}

		var in struct {
			Body json.RawMessage `json:"body"`
		}
		if err := json.Unmarshal(c.Body(), &in); err != nil {
			return writeRaw(c, fiber.StatusBadRequest, errMalformedJSON)
		}

		var raw string
		if len(in.Body) == 0 || in.Body[0] != '"' || json.Unmarshal(in.Body, &raw) != nil {
			return writeRaw(c, fiber.StatusBadRequest, errBodyRequired)
		}
		body := strings.TrimSpace(raw)
		if body == "" {
			return writeRaw(c, fiber.StatusBadRequest, errBodyRequired)
		}
		if utf8.RuneCountInString(body) > 500 {
			return writeRaw(c, fiber.StatusBadRequest, errBodyTooLong)
		}

		dc := pool.get()
		id, createdAt, err := dc.createPost(user.ID, body)
		pool.put(dc)
		if err != nil {
			return writeRaw(c, fiber.StatusInternalServerError, errInternal)
		}

		p := Post{
			ID:        id,
			Body:      body,
			CreatedAt: createdAt,
			Author:    user.Username,
			LikeCount: 0,
		}
		buf := acquireJSONBuf()
		b := append(*buf, `{"post":`...)
		b = appendPostJSON(b, &p)
		b = append(b, '}')
		*buf = b
		return writeJSON(c, fiber.StatusCreated, buf)
	})

	app.Post("/posts/:id/like", func(c fiber.Ctx) error {
		// Auth is checked before the id, as the spec requires.
		user, ok := verifier.requireUser(c)
		if !ok {
			return nil
		}

		id, ok := parsePostID(c.Params("id"))
		if !ok {
			return writeRaw(c, fiber.StatusBadRequest, errInvalidPostID)
		}

		dc := pool.get()
		n, err := dc.createLike(user.ID, id)
		if err != nil {
			pool.put(dc)
			return writeRaw(c, fiber.StatusInternalServerError, errInternal)
		}
		if n == 1 {
			pool.put(dc)
			return writeLike(c, fiber.StatusCreated, false, id)
		}

		exists, err := dc.existsPost(id)
		pool.put(dc)
		if err != nil {
			return writeRaw(c, fiber.StatusInternalServerError, errInternal)
		}
		if !exists {
			return writeRaw(c, fiber.StatusNotFound, errPostNotFound)
		}
		return writeLike(c, fiber.StatusOK, true, id)
	})

	addr := net.JoinHostPort(envOr("HOST", "127.0.0.1"), envOr("PORT", "3000"))
	log.Printf("listening on %s", addr)
	if err := app.Listen(addr, fiber.ListenConfig{DisableStartupMessage: true}); err != nil {
		log.Fatalf("error starting the API: %v", err)
	}
}
