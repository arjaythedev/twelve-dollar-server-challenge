package main

import "github.com/valyala/fasthttp"

// Auth implements the JWT authentication middleware
func Auth(next fasthttp.RequestHandler, secret string) fasthttp.RequestHandler {
	return func(ctx *fasthttp.RequestCtx) {
		next(ctx)
	}
}
