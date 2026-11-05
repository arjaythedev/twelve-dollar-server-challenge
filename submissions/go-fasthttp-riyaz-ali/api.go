package main

import (
	"crawshaw.io/sqlite/sqlitex"
	"github.com/valyala/fasthttp"
)

func Health(pool *sqlitex.Pool) fasthttp.RequestHandler {
	return func(ctx *fasthttp.RequestCtx) {
		ctx.SetStatusCode(200)
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
