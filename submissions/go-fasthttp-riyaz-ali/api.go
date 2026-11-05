package main

import (
	"crawshaw.io/sqlite"
	"crawshaw.io/sqlite/sqlitex"
	"fmt"
	"github.com/valyala/fasthttp"
	"io"
	"strconv"
	"time"
)

func Health(pool *sqlitex.Pool) fasthttp.RequestHandler {
	var started = time.Now()
	const query = "SELECT 1"

	return func(ctx *fasthttp.RequestCtx) {
		conn := pool.Get(ctx)
		defer pool.Put(conn)

		stmt := conn.Prep(query)
		defer clearAndReset(stmt)

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
	const query = `
		WITH serialized_posts AS (
			SELECT json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)) AS val
            FROM posts p
                JOIN users u ON u.id = p.user_id
            ORDER BY p.created_at DESC, p.id DESC 
            LIMIT 20
        ) SELECT json_object('posts', json_group_array(json(val))) FROM serialized_posts`

	return func(ctx *fasthttp.RequestCtx) {
		conn := pool.Get(ctx)
		defer pool.Put(conn)

		stmt := conn.Prep(query)
		defer clearAndReset(stmt)

		if ok, err := stmt.Step(); err != nil {
			erred(ctx, fasthttp.StatusInternalServerError, err)
		} else if ok {
			ctx.SetStatusCode(fasthttp.StatusOK)
			ctx.SetContentType("application/json")

			if _, err = io.Copy(ctx, stmt.ColumnReader(0)); err != nil {
				erred(ctx, fasthttp.StatusInternalServerError, err)
			}
		}
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

func clearAndReset(stmt *sqlite.Stmt) (err error) {
	if err = stmt.ClearBindings(); err == nil {
		err = stmt.Reset()
	}

	return err
}

func erred(ctx *fasthttp.RequestCtx, code int, err error) {
	ctx.ResetBody()
	ctx.SetStatusCode(code)
	_, _ = fmt.Fprintf(ctx, `{"error":%s}`, strconv.Quote(err.Error()))
}
