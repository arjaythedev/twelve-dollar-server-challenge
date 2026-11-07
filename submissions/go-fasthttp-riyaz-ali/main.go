// This application implements a FastHTTP-based server in Golang with sqlite3 as the datastore
// for the $12-server challenge (see: https://benchmarks.arjaythedev.com/12-dollar-server)
package main

import (
	"errors"
	"log"
	"net"
	"runtime/debug"
	"syscall"
	"time"

	"github.com/fasthttp/router"
	"github.com/valyala/fasthttp"
)

// maxOpenConns is the number of open connections above which responses carry
// "Connection: close", so keep-alive clients hand their fd back and reconnect on their
// next request. It is 7/8 of the fd limit (57k with the benchmark's LimitNOFILE=65535),
// leaving headroom for connections that are mid-request.
func maxOpenConns() int32 {
	var rl syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &rl); err != nil || rl.Cur == 0 {
		return 48_000
	}
	return int32(min(rl.Cur*7/8, 1<<30))
}

func main() {
	secret, path := env("JWT_SECRET", "twelve-dollar-challenge"), env("SQLITE_PATH", "seed/feed.db")

	// Fewer, larger GCs: the heap is small and the box has 2 GB. The soft limit keeps
	// us honest if connections pile up.
	debug.SetGCPercent(400)
	debug.SetMemoryLimit(1200 << 20)

	db := must(OpenDB(path))
	defer db.Close()

	r := router.New()
	r.GET("/health", Health(db))
	r.GET("/feed", Feed(db))
	r.GET("/posts/{id}", GetPost(db))
	r.POST("/posts", Auth(CreatePost(db), secret))
	r.POST("/posts/{id}/like", Auth(LikePost(db), secret))

	// configure not-found handler
	r.HandleMethodNotAllowed = false
	r.NotFound = NotFound()

	maxConns := maxOpenConns()
	server := &fasthttp.Server{
		Name:                          "",
		NoDefaultServerHeader:         true,
		NoDefaultDate:                 true,
		DisablePreParseMultipartForm:  true,
		ReduceMemoryUsage:             true, // release per-connection buffers while a connection is idle
		ReadBufferSize:                4096,
		WriteBufferSize:               8192, // a feed response fits in one write
		MaxRequestBodySize:            64 << 10,
		IdleTimeout:                   30 * time.Second, // direct mode only: drops connections of users who left
		CloseOnShutdown:               true,
		SecureErrorLogMessage:         true,
		DisableHeaderNamesNormalizing: false,
	}

	server.Handler = func(ctx *fasthttp.RequestCtx) {
		if server.GetOpenConnectionsCount() > maxConns {
			ctx.SetConnectionClose()
		}
		r.Handler(ctx)
	}

	addr := net.JoinHostPort(env("HOST", "127.0.0.1"), env("PORT", "3000"))
	ln := must(net.Listen("tcp", addr))
	log.Printf("listening on %s (keep-alive up to %d connections)", addr, maxConns)

	if err := server.Serve(retryListener{ln}); err != nil {
		log.Fatal(err.Error())
	}
}

// retryListener keeps accepting when the process runs out of file descriptors,
// instead of returning the error (which makes fasthttp's Serve exit).
type retryListener struct{ net.Listener }

func (l retryListener) Accept() (net.Conn, error) {
	for {
		c, err := l.Listener.Accept()
		if err != nil && (errors.Is(err, syscall.EMFILE) || errors.Is(err, syscall.ENFILE)) {
			time.Sleep(5 * time.Millisecond)
			continue
		}
		return c, err
	}
}
