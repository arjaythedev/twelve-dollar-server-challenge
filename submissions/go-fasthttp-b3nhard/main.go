package main

import (
	"bytes"
	"log"
	"math"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"sync"
	"time"
	"unicode/utf8"
	"unsafe"

	"github.com/valyala/fasthttp"
)

var (
	db    *DB
	start time.Time
)

var (
	pathHealth   = []byte("/health")
	pathFeed     = []byte("/feed")
	pathPosts    = []byte("/posts")
	prefixPosts  = []byte("/posts/")
	suffixLike   = []byte("/like")
	bearerPrefix = []byte("Bearer ")
)

// Static error bodies: byte-identical to SPEC.md and allocation-free.
var (
	errNotFound      = []byte(`{"error":"not found"}`)
	errInvalidID     = []byte(`{"error":"invalid post id"}`)
	errPostNotFound  = []byte(`{"error":"post not found"}`)
	errMissingBearer = []byte(`{"error":"missing bearer token"}`)
	errBadToken      = []byte(`{"error":"invalid or expired token"}`)
	errBadPayload    = []byte(`{"error":"invalid token payload"}`)
	errMalformedJSON = []byte(`{"error":"malformed JSON body"}`)
	errBodyRequired  = []byte(`{"error":"body is required"}`)
	errBodyTooLong   = []byte(`{"error":"body must be at most 500 characters"}`)
	errInternal      = []byte(`{"error":"internal server error"}`)
)

// trimSet is JavaScript's String.prototype.trim() whitespace set, so bodies are
// trimmed exactly like the reference implementations.
const trimSet = "\t\n\v\f\r \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff"

var bufPool = sync.Pool{New: func() any {
	b := make([]byte, 0, 8192)
	return &b
}}

func acquireBuf() *[]byte { return bufPool.Get().(*[]byte) }

func releaseBuf(b *[]byte) {
	if cap(*b) <= 1<<20 {
		*b = (*b)[:0]
		bufPool.Put(b)
	}
}

type nopLogger struct{}

func (nopLogger) Printf(string, ...any) {}

func respond(ctx *fasthttp.RequestCtx, status int, body []byte) {
	ctx.SetStatusCode(status)
	ctx.SetContentType("application/json")
	ctx.SetBody(body)
}

func bytesOfString(s string) []byte {
	if len(s) == 0 {
		return nil
	}
	return unsafe.Slice(unsafe.StringData(s), len(s))
}

// parseID accepts a positive ASCII integer. ok is false for anything else; when
// the digits overflow int64, ok is true with overflow set (an id that cannot exist).
func parseID(s []byte) (id int64, ok, overflow bool) {
	if len(s) == 0 {
		return 0, false, false
	}
	var n int64
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false, false
		}
		d := int64(c - '0')
		if n > (math.MaxInt64-d)/10 {
			return 0, true, true
		}
		n = n*10 + d
	}
	if n == 0 {
		return 0, false, false
	}
	return n, true, false
}

func handler(ctx *fasthttp.RequestCtx) {
	defer func() {
		if recover() != nil {
			respond(ctx, 500, errInternal)
		}
	}()

	path := ctx.Path()
	switch {
	case ctx.IsGet():
		switch {
		case len(path) == len(pathHealth) && bytes.Equal(path, pathHealth):
			handleHealth(ctx)
		case len(path) == len(pathFeed) && bytes.Equal(path, pathFeed):
			handleFeed(ctx)
		case len(path) > len(prefixPosts) && bytes.HasPrefix(path, prefixPosts):
			handleGetPost(ctx, path[len(prefixPosts):])
		default:
			respond(ctx, 404, errNotFound)
		}
	case ctx.IsPost():
		switch {
		case len(path) == len(pathPosts) && bytes.Equal(path, pathPosts):
			handleCreate(ctx)
		case len(path) > len(prefixPosts)+len(suffixLike) &&
			bytes.HasPrefix(path, prefixPosts) && bytes.HasSuffix(path, suffixLike):
			handleLike(ctx, path[len(prefixPosts):len(path)-len(suffixLike)])
		default:
			respond(ctx, 404, errNotFound)
		}
	default:
		respond(ctx, 404, errNotFound)
	}
}

func handleHealth(ctx *fasthttp.RequestCtx) {
	if err := db.Health(); err != nil {
		bp := acquireBuf()
		*bp = append(*bp, `{"status":"degraded","db":"unreachable","error":`...)
		*bp = appendJSONString(*bp, bytesOfString(err.Error()))
		*bp = append(*bp, '}')
		respond(ctx, 503, *bp)
		releaseBuf(bp)
		return
	}
	bp := acquireBuf()
	*bp = append(*bp, `{"status":"ok","db":"ok","uptime_s":`...)
	*bp = strconv.AppendInt(*bp, int64(time.Since(start)/time.Second), 10)
	*bp = append(*bp, '}')
	respond(ctx, 200, *bp)
	releaseBuf(bp)
}

func handleFeed(ctx *fasthttp.RequestCtx) {
	bp := acquireBuf()
	b, err := db.AppendFeed(*bp)
	if err != nil {
		releaseBuf(bp)
		respond(ctx, 500, errInternal)
		return
	}
	*bp = b
	respond(ctx, 200, *bp)
	releaseBuf(bp)
}

func handleGetPost(ctx *fasthttp.RequestCtx, raw []byte) {
	id, ok, overflow := parseID(raw)
	if !ok {
		respond(ctx, 400, errInvalidID)
		return
	}
	if overflow {
		respond(ctx, 404, errPostNotFound)
		return
	}
	bp := acquireBuf()
	b, found, err := db.AppendGetPost(*bp, id)
	if err != nil {
		releaseBuf(bp)
		respond(ctx, 500, errInternal)
		return
	}
	if !found {
		releaseBuf(bp)
		respond(ctx, 404, errPostNotFound)
		return
	}
	*bp = b
	respond(ctx, 200, *bp)
	releaseBuf(bp)
}

func handleCreate(ctx *fasthttp.RequestCtx) {
	sc := acquireScratch()
	defer releaseScratch(sc)
	userID, username, ok := authenticate(ctx, sc)
	if !ok {
		return
	}
	body, status := extractBodyJSON(ctx.PostBody())
	switch status {
	case bodyMalformed:
		respond(ctx, 400, errMalformedJSON)
		return
	case bodyRequired:
		respond(ctx, 400, errBodyRequired)
		return
	}
	body = bytes.Trim(body, trimSet)
	if len(body) == 0 {
		respond(ctx, 400, errBodyRequired)
		return
	}
	if utf8.RuneCount(body) > 500 {
		respond(ctx, 400, errBodyTooLong)
		return
	}
	bp := acquireBuf()
	b, err := db.AppendCreatePost(*bp, userID, body, username)
	if err != nil {
		releaseBuf(bp)
		respond(ctx, 500, errInternal)
		return
	}
	*bp = b
	respond(ctx, 201, *bp)
	releaseBuf(bp)
}

func handleLike(ctx *fasthttp.RequestCtx, raw []byte) {
	sc := acquireScratch()
	defer releaseScratch(sc)
	userID, _, ok := authenticate(ctx, sc)
	if !ok {
		return
	}
	id, valid, overflow := parseID(raw)
	if !valid {
		respond(ctx, 400, errInvalidID)
		return
	}
	if overflow {
		respond(ctx, 404, errPostNotFound)
		return
	}
	inserted, exists, err := db.InsertLike(userID, id)
	if err != nil {
		respond(ctx, 500, errInternal)
		return
	}
	switch {
	case inserted:
		bp := acquireBuf()
		*bp = append(*bp, `{"liked":true,"already_liked":false,"post_id":`...)
		*bp = strconv.AppendInt(*bp, id, 10)
		*bp = append(*bp, '}')
		respond(ctx, 201, *bp)
		releaseBuf(bp)
	case exists:
		bp := acquireBuf()
		*bp = append(*bp, `{"liked":true,"already_liked":true,"post_id":`...)
		*bp = strconv.AppendInt(*bp, id, 10)
		*bp = append(*bp, '}')
		respond(ctx, 200, *bp)
		releaseBuf(bp)
	default:
		respond(ctx, 404, errPostNotFound)
	}
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	dbPath := os.Getenv("SQLITE_PATH")
	if dbPath == "" {
		log.Fatal("SQLITE_PATH is required")
	}
	secret := os.Getenv("JWT_SECRET")
	if secret == "" {
		log.Fatal("JWT_SECRET is required")
	}
	host := getenv("HOST", "127.0.0.1")
	port := getenv("PORT", "3000")

	// The request path is allocation-free, so the collector has almost nothing to
	// do. Run it only when the Go heap approaches the soft limit: GC stays out of
	// the latency tail while memory stays bounded well under the 2 GB ceiling.
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(512 << 20)

	reads := runtime.GOMAXPROCS(0)
	if reads < 4 {
		reads = 4
	}
	var err error
	db, err = Open(dbPath, reads)
	if err != nil {
		log.Fatalf("open db: %v", err)
	}
	initHMAC([]byte(secret))
	start = time.Now()
	go db.CheckpointLoop(5 * time.Second)

	srv := &fasthttp.Server{
		Handler: handler,
		// No per-request read deadlines: SPEC only requires idle connections to be
		// held open (>= 65 s), and never timing them out satisfies that while
		// removing a timer arm/cancel from every request and keep-alive wait.
		// ReduceMemoryUsage releases per-connection buffers while idle, which is
		// what makes ~15k open connections affordable.
		LazyRequestTime:       true,
		ReduceMemoryUsage:     true,
		ReadBufferSize:        4096,
		WriteBufferSize:       16384,
		MaxRequestBodySize:    1 << 20,
		NoDefaultServerHeader: true,
		NoDefaultDate:         true,
		NoDefaultContentType:  true,
		Logger:                nopLogger{},
	}
	if err := srv.ListenAndServe(host + ":" + port); err != nil {
		log.Fatal(err)
	}
}
