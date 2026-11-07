// This application implements a FastHTTP-based server in Golang with sqlite3 as the datastore
// for the $12-server challenge (see: https://benchmarks.arjaythedev.com/12-dollar-server)
package main

import (
	"context"
	"crawshaw.io/sqlite"
	"crawshaw.io/sqlite/sqlitex"
	"github.com/fasthttp/router"
	"github.com/valyala/fasthttp"
	"log"
	"net"
	"runtime"
)

const pragmas = `
PRAGMA temp_store = MEMORY;
PRAGMA mmap_size = 268435456;
PRAGMA cache_size = -65536;
`

func main() {
	secret, path := env("JWT_SECRET", "twelve-dollar-challenge"), env("SQLITE_PATH", "seed/feed.db")

	ctx := context.Background()
	r := router.New()

	reader := must(sqlitex.OpenInit(ctx, path, sqlite.OpenFlagsDefault, runtime.NumCPU()*2, pragmas))
	defer reader.Close()

	r.GET("/health", Health(reader))
	r.GET("/feed", Feed(reader))
	r.GET("/posts/{id}", GetPost(reader))

	writer := must(sqlitex.OpenInit(ctx, path, sqlite.OpenFlagsDefault, runtime.NumCPU()*2, pragmas))
	defer writer.Close()

	r.POST("/posts", Auth(CreatePost(writer), secret))
	r.POST("/posts/{id}/like", Auth(LikePost(writer), secret))

	// configure not-found handler
	r.HandleMethodNotAllowed = false
	r.NotFound = NotFound()

	addr := net.JoinHostPort(env("HOST", "127.0.0.1"), env("PORT", "3000"))
	log.Printf("listening on %s", addr)

	if err := fasthttp.ListenAndServe(addr, r.Handler); err != nil {
		log.Fatal(err.Error())
	}
}
