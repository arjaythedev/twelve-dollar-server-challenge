package main

import (
	"bytes"
	"crawshaw.io/sqlite"
	"crawshaw.io/sqlite/sqlitex"
	"errors"
	"fmt"
	"github.com/valyala/fasthttp"
	"github.com/valyala/fastjson"
	"io"
	"strconv"
	"strings"
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
			_, _ = fmt.Fprintf(ctx, `{"status":"ok","db":"ok","uptime_s":%d}`, int64(time.Since(started).Seconds()))
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
	const query = `
	SELECT json_object('post', 
		json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)))
	FROM posts p
	    JOIN users u ON u.id = p.user_id
	WHERE p.id = $1`

	return func(ctx *fasthttp.RequestCtx) {
		id, err := strconv.ParseInt(ctx.UserValue("id").(string), 10, 64)
		if err != nil || id < 1 {
			erred(ctx, fasthttp.StatusBadRequest, errors.New("invalid post id"))
			return
		}

		conn := pool.Get(ctx)
		defer pool.Put(conn)

		stmt := conn.Prep(query)
		defer clearAndReset(stmt)

		stmt.BindInt64(1, id)
		ok, err := stmt.Step()
		if err != nil {
			erred(ctx, fasthttp.StatusInternalServerError, err)
		} else if !ok {
			erred(ctx, fasthttp.StatusNotFound, errors.New("post not found"))
		} else {
			ctx.SetStatusCode(fasthttp.StatusOK)
			ctx.SetContentType("application/json")

			if _, err = io.Copy(ctx, stmt.ColumnReader(0)); err != nil {
				erred(ctx, fasthttp.StatusInternalServerError, err)
			}
		}
	}
}

func CreatePost(pool *sqlitex.Pool) fasthttp.RequestHandler {
	var parsers fastjson.ParserPool

	const query = `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING (
    	json_object('post', 
    		json_object('id', id, 'body', body, 'created_at', created_at, 'author', (SELECT username FROM users WHERE id = user_id), 'like_count', 0)
    	)
    )`

	return func(ctx *fasthttp.RequestCtx) {
		parser := parsers.Get()
		defer parsers.Put(parser)

		val, err := parser.ParseBytes(ctx.PostBody())
		if err != nil {
			erred(ctx, fasthttp.StatusBadRequest, errors.New("malformed JSON body"))
			return
		}

		var body strings.Builder
		if buf := val.GetStringBytes("body"); buf == nil {
			erred(ctx, fasthttp.StatusBadRequest, errors.New("body is required"))
			return
		} else {
			body.Write(buf)
		}

		trimmed := strings.TrimSpace(body.String())
		if trimmed == "" {
			erred(ctx, fasthttp.StatusBadRequest, errors.New("body is required"))
			return
		} else if len(trimmed) > 500 {
			erred(ctx, fasthttp.StatusBadRequest, errors.New("body must be at most 500 characters"))
			return
		}

		conn := pool.Get(ctx)
		defer pool.Put(conn)

		stmt := conn.Prep(query)
		defer clearAndReset(stmt)

		stmt.BindInt64(1, 1) // TODO: get user_id from auth token
		stmt.BindText(2, trimmed)

		ok, err := stmt.Step()
		if err != nil {
			erred(ctx, fasthttp.StatusInternalServerError, err)
		} else if !ok {
			erred(ctx, fasthttp.StatusNotFound, errors.New("internal server error"))
		} else {
			ctx.SetStatusCode(fasthttp.StatusCreated)
			ctx.SetContentType("application/json")

			if _, err = io.Copy(ctx, stmt.ColumnReader(0)); err != nil {
				erred(ctx, fasthttp.StatusInternalServerError, err)
			}
		}
	}
}

func LikePost(pool *sqlitex.Pool) fasthttp.RequestHandler {
	var arenas fastjson.ArenaPool
	const query = `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT DO NOTHING`

	return func(ctx *fasthttp.RequestCtx) {
		id, err := strconv.ParseInt(ctx.UserValue("id").(string), 10, 64)
		if err != nil || id < 1 {
			erred(ctx, fasthttp.StatusBadRequest, errors.New("invalid post id"))
			return
		}

		conn := pool.Get(ctx)
		defer pool.Put(conn)

		stmt := conn.Prep(query)
		defer clearAndReset(stmt)

		stmt.BindInt64(1, 1) // TODO: get user_id from auth token
		stmt.BindInt64(2, id)

		ok, err := stmt.Step()
		if err != nil {
			if sqlite.ErrCode(err) == sqlite.SQLITE_CONSTRAINT_FOREIGNKEY {
				erred(ctx, fasthttp.StatusNotFound, errors.New("post not found"))
			} else {
				erred(ctx, fasthttp.StatusInternalServerError, err)
			}
		} else {
			ctx.SetContentType("application/json")

			arena := arenas.Get()
			defer arenas.Put(arena)

			ret := arena.NewObject()
			ret.Set("liked", arena.NewTrue())
			if !ok {
				ctx.SetStatusCode(fasthttp.StatusOK)
				ret.Set("already_liked", arena.NewTrue())
			} else {
				ctx.SetStatusCode(fasthttp.StatusCreated)
				ret.Set("already_liked", arena.NewFalse())
			}
			ret.Set("post_id", arena.NewNumberInt(int(id)))

			if _, err = io.Copy(ctx, bytes.NewReader(ret.MarshalTo(nil))); err != nil {
				erred(ctx, fasthttp.StatusInternalServerError, err)
			}
		}
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
