package main

import (
	"bytes"
	"log"
	"os"
	"os/signal"
	"runtime"
	"runtime/pprof"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/valyala/fasthttp"
)

var (
	pathHealth   = []byte("/health")
	pathFeed     = []byte("/feed")
	pathPosts    = []byte("/posts")
	prefixPosts  = []byte("/posts/")
	suffixLike   = []byte("/like")
	hdrAuth      = []byte("Authorization")
	bearerPrefix = []byte("Bearer ")
	ctJSON       = []byte("application/json")
)

// Error bodies are byte-exact per SPEC.md and never allocated.
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

type server struct {
	db    *db
	key   *hmacKey
	start time.Time
}

var reqPool = sync.Pool{New: func() any {
	return &dbReq{out: make([]byte, 0, outCap), done: make(chan struct{}, 1)}
}}

var scratchPool = sync.Pool{New: func() any {
	b := make([]byte, 0, 2048)
	return &b
}}

func (s *server) run(r *dbReq) {
	s.db.ch <- r
	<-r.done
}

func release(r *dbReq) {
	if cap(r.out) <= 1<<20 {
		r.out, r.body, r.name, r.status = r.out[:0], nil, nil, 0
		reqPool.Put(r)
	}
}

func respond(ctx *fasthttp.RequestCtx, status int, body []byte) {
	ctx.SetStatusCode(status)
	ctx.Response.Header.SetContentTypeBytes(ctJSON)
	ctx.SetBody(body)
}

func (s *server) handler(ctx *fasthttp.RequestCtx) {
	defer func() {
		if recover() != nil {
			respond(ctx, 500, errInternal)
		}
	}()
	path := ctx.Path()
	if ctx.IsGet() {
		switch {
		case bytes.Equal(path, pathFeed):
			s.read(ctx, reqFeed, 0)
		case bytes.HasPrefix(path, prefixPosts):
			id, ok := parseID(path[len(prefixPosts):])
			if !ok {
				respond(ctx, 400, errInvalidID)
				return
			}
			s.read(ctx, reqPost, id)
		case bytes.Equal(path, pathHealth):
			s.health(ctx)
		default:
			respond(ctx, 404, errNotFound)
		}
		return
	}
	if ctx.IsPost() {
		switch {
		case bytes.Equal(path, pathPosts):
			s.create(ctx)
		case bytes.HasPrefix(path, prefixPosts) && bytes.HasSuffix(path, suffixLike):
			s.like(ctx, path[len(prefixPosts):len(path)-len(suffixLike)])
		default:
			respond(ctx, 404, errNotFound)
		}
		return
	}
	respond(ctx, 404, errNotFound)
}

func (s *server) read(ctx *fasthttp.RequestCtx, kind reqKind, id int64) {
	r := reqPool.Get().(*dbReq)
	r.kind, r.id = kind, id
	s.run(r)
	switch r.status {
	case 200:
		respond(ctx, 200, r.out)
	case 404:
		respond(ctx, 404, errPostNotFound)
	default:
		respond(ctx, 500, errInternal)
	}
	release(r)
}

func (s *server) health(ctx *fasthttp.RequestCtx) {
	r := reqPool.Get().(*dbReq)
	r.kind = reqPing
	s.run(r)
	if r.status != 200 {
		r.out = append(r.out[:0], `{"status":"degraded","db":"unreachable","error":"sqlite error"}`...)
		respond(ctx, 503, r.out)
	} else {
		r.out = append(r.out[:0], `{"status":"ok","db":"ok","uptime_s":`...)
		r.out = strconv.AppendInt(r.out, int64(time.Since(s.start)/time.Second), 10)
		r.out = append(r.out, '}')
		respond(ctx, 200, r.out)
	}
	release(r)
}

// auth verifies the bearer token. On failure it has already written the response.
func (s *server) auth(ctx *fasthttp.RequestCtx, scratch *[]byte) (int64, []byte, bool) {
	h := ctx.Request.Header.PeekBytes(hdrAuth)
	if !bytes.HasPrefix(h, bearerPrefix) {
		respond(ctx, 401, errMissingBearer)
		return 0, nil, false
	}
	uid, name, err := verifyJWT(h[len(bearerPrefix):], s.key, scratch, time.Now().Unix())
	switch err {
	case jwtInvalid:
		respond(ctx, 401, errBadToken)
		return 0, nil, false
	case jwtPayload:
		respond(ctx, 401, errBadPayload)
		return 0, nil, false
	}
	return uid, name, true
}

func (s *server) create(ctx *fasthttp.RequestCtx) {
	scratch := scratchPool.Get().(*[]byte)
	defer scratchPool.Put(scratch)
	uid, name, ok := s.auth(ctx, scratch)
	if !ok {
		return
	}
	r := reqPool.Get().(*dbReq)
	defer release(r)
	// the unescaped body goes into r.out's spare capacity; the DB appends the response after it
	// is no longer needed, so reuse the same allocation via a second pooled scratch
	bodyBuf := scratchPool.Get().(*[]byte)
	defer scratchPool.Put(bodyBuf)
	body, berr := parseBody(ctx.PostBody(), bodyBuf)
	switch berr {
	case bodyMalformed:
		respond(ctx, 400, errMalformedJSON)
		return
	case bodyRequired:
		respond(ctx, 400, errBodyRequired)
		return
	case bodyTooLong:
		respond(ctx, 400, errBodyTooLong)
		return
	}
	r.kind, r.user, r.name, r.body = reqCreate, uid, name, body
	s.run(r)
	if r.status != 201 {
		respond(ctx, 500, errInternal)
		return
	}
	respond(ctx, 201, r.out)
}

func (s *server) like(ctx *fasthttp.RequestCtx, rawID []byte) {
	scratch := scratchPool.Get().(*[]byte)
	defer scratchPool.Put(scratch)
	uid, _, ok := s.auth(ctx, scratch)
	if !ok {
		return
	}
	id, ok := parseID(rawID)
	if !ok {
		respond(ctx, 400, errInvalidID)
		return
	}
	r := reqPool.Get().(*dbReq)
	defer release(r)
	r.kind, r.user, r.id = reqLike, uid, id
	s.run(r)
	switch r.status {
	case 200, 201:
		respond(ctx, r.status, r.out)
	case 404:
		respond(ctx, 404, errPostNotFound)
	default:
		respond(ctx, 500, errInternal)
	}
}

type nopLogger struct{}

func (nopLogger) Printf(string, ...any) {}

func env(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func main() {
	d, err := openDB(env("SQLITE_PATH", "feed.db"))
	if err != nil {
		log.Fatal(err)
	}
	s := &server{db: d, key: newHMACKey([]byte(env("JWT_SECRET", ""))), start: time.Now()}

	if dir := os.Getenv("PROFILE_DIR"); dir != "" {
		go profileOnSignal(dir)
	}
	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
		<-sig
		stopProfile()
		gcovDump()
		os.Exit(0)
	}()

	srv := &fasthttp.Server{
		Handler:                       s.handler,
		Name:                          "",
		ReadBufferSize:                4096,
		WriteBufferSize:               8192,
		MaxRequestBodySize:            16 << 10,
		ReduceMemoryUsage:             true,
		NoDefaultServerHeader:         true,
		NoDefaultDate:                 true,
		NoDefaultContentType:          true,
		DisableHeaderNamesNormalizing: true,
		Logger:                        nopLogger{},
	}
	addr := env("HOST", "0.0.0.0") + ":" + env("PORT", "3000")
	log.Printf("listening on %s", addr)
	if err := srv.ListenAndServe(addr); err != nil {
		log.Fatal(err)
	}
}

var profMu sync.Mutex
var profFile *os.File

// profileOnSignal toggles a CPU profile on each SIGUSR1 (only when PROFILE_DIR is set); stopping
// also writes a heap profile next to it.
func profileOnSignal(dir string) {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGUSR1)
	for range sig {
		profMu.Lock()
		if profFile == nil {
			f, err := os.Create(dir + "/cpu-" + strconv.FormatInt(time.Now().Unix(), 10) + ".pprof")
			if err == nil && pprof.StartCPUProfile(f) == nil {
				profFile = f
				log.Printf("profiling to %s", f.Name())
			}
		} else {
			finishProfile()
		}
		profMu.Unlock()
	}
}

// finishProfile closes the CPU profile and dumps the heap; caller holds profMu.
func finishProfile() {
	pprof.StopCPUProfile()
	profFile.Close()
	log.Printf("profile written to %s", profFile.Name())
	name := profFile.Name()
	profFile = nil
	// With GOGC=off the heap profile is only as fresh as the last GC, so force one.
	runtime.GC()
	if f, err := os.Create(strings.Replace(name, "/cpu-", "/heap-", 1)); err == nil {
		if err := pprof.Lookup("allocs").WriteTo(f, 0); err != nil {
			log.Printf("heap profile: %v", err)
		}
		f.Close()
		log.Printf("heap profile written to %s", f.Name())
	}
}

func stopProfile() {
	profMu.Lock()
	if profFile != nil {
		finishProfile()
	}
	profMu.Unlock()
}
