package main

import (
	"crawshaw.io/sqlite/sqlitex"
	"fmt"
	"github.com/valyala/fasthttp"
	"time"
)

func Health(pool *sqlitex.Pool) fasthttp.RequestHandler {
	var started = time.Now()
	const query = "SELECT 1"

	return func(ctx *fasthttp.RequestCtx) {
		conn := pool.Get(ctx)
		defer pool.Put(conn)

		stmt := conn.Prep(query)
		defer stmt.Reset()

		if ok, err := stmt.Step(); err != nil || !ok {
			ctx.SetStatusCode(fasthttp.StatusServiceUnavailable)
			_, _ = fmt.Fprintf(ctx, `{"status":"degraded","db":"unreachable","error":"internal server error"}`)
		} else {
			ctx.SetStatusCode(fasthttp.StatusOK)
			_, _ = fmt.Fprintf(ctx, `{"status":"ok","db":"ok","uptime_s": %d}`, int64(time.Since(started).Seconds()))
		}
	}
}

func Feed(pool *sqlitex.Pool) fasthttp.RequestHandler {
	return func(ctx *fasthttp.RequestCtx) {
		ctx.SetStatusCode(200)
	}
}

func GetPost(pool *sqlitex.Pool) fasthttp.RequestHandler {
	return func(ctx *fasthttp.RequestCtx) {
		ctx.SetStatusCode(200)
	}
}

func CreatePost(pool *sqlitex.Pool) fasthttp.RequestHandler {
	return func(ctx *fasthttp.RequestCtx) {
		ctx.SetStatusCode(200)
	}
}

func LikePost(pool *sqlitex.Pool) fasthttp.RequestHandler {
	return func(ctx *fasthttp.RequestCtx) {
		ctx.SetStatusCode(200)
	}
}
