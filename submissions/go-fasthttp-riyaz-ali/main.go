// This application implements a FastHTTP-based server in Golang with sqlite3 as the datastore
// for the $12-server challenge (see: https://benchmarks.arjaythedev.com/12-dollar-server)
package main

import (
	"crawshaw.io/sqlite"
	"crawshaw.io/sqlite/sqlitex"
	"github.com/fasthttp/router"
	"github.com/valyala/fasthttp"
	"log"
	"net"
	"runtime"
)

func main() {
	secret, path := env("JWT_SECRET", "twelve-dollar-challenge"), env("SQLITE_PATH", "seed/feed.db")

	pool := must(sqlitex.Open(path, sqlite.OpenFlagsDefault, runtime.NumCPU()*2))
	defer pool.Close()

	r := router.New()
	r.GET("/health", Health(pool))
	r.GET("/feed", Feed(pool))
	r.GET("/posts/{id}", GetPost(pool))
	r.POST("/posts", Auth(CreatePost(pool), secret))
	r.POST("/posts/{id}/like", Auth(LikePost(pool), secret))

	addr := net.JoinHostPort(env("HOST", "127.0.0.1"), env("PORT", "3000"))
	log.Printf("listening on %s", addr)

	if err := fasthttp.ListenAndServe(addr, r.Handler); err != nil {
		log.Fatal(err.Error())
	}
}
