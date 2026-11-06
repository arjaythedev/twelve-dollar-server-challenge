package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"github.com/valyala/fasthttp"
	"github.com/valyala/fastjson"
	"hash"
	"io"
	"strconv"
	"strings"
	"sync"
	"time"
	"unsafe"
)

// Auth implements the JWT authentication middleware
func Auth(next fasthttp.RequestHandler, secret string) fasthttp.RequestHandler {
	bearer, hs256 := []byte("Bearer "), []byte("HS256")

	sec := unsafe.Slice(unsafe.StringData(secret), len(secret))
	pool := sync.Pool{New: func() any { return hmac.New(sha256.New, sec) }}

	var parsers fastjson.ParserPool // for parsing json claims

	// adapted from go-gin-lincolng4's implementation
	var verify = func(auth []byte) (int64, string, error) {
		invalidToken := errors.New("invalid or expired token")

		if !bytes.HasPrefix(auth, bearer) {
			return 0, "", errors.New("missing bearer token")
		}

		token := string(auth[len(bearer):])
		i, j := strings.IndexByte(token, '.'), strings.LastIndexByte(token, '.')
		if i <= 0 || j <= i+1 || j == len(token)-1 {
			return 0, "", invalidToken
		}
		if strings.IndexByte(token[i+1:j], '.') >= 0 { // more than 3 parts
			return 0, "", invalidToken
		}

		var want []byte
		{ // compute the hash
			m := pool.Get().(hash.Hash)
			m.Reset()
			_, _ = io.WriteString(m, token[:j])
			want = m.Sum(nil)
			pool.Put(m)
		}

		if sig, err := base64.RawURLEncoding.DecodeString(token[j+1:]); err != nil || !hmac.Equal(sig, want) {
			return 0, "", invalidToken
		}

		var parser = parsers.Get()
		defer parsers.Put(parser)

		var header *fastjson.Value
		if raw, err := base64.RawURLEncoding.DecodeString(token[:i]); err != nil {
			return 0, "", invalidToken
		} else if header, err = parser.ParseBytes(raw); err != nil {
			return 0, "", invalidToken
		} else if !bytes.Equal(header.GetStringBytes("alg"), hs256) {
			return 0, "", invalidToken
		}

		var payload *fastjson.Value
		if raw, err := base64.RawURLEncoding.DecodeString(token[i+1 : j]); err != nil {
			return 0, "", invalidToken
		} else if payload, err = parser.ParseBytes(raw); err != nil {
			return 0, "", invalidToken
		}

		if exp := payload.GetInt64("exp"); exp <= time.Now().Unix() { // missing exp counts as expired
			return 0, "", invalidToken
		}

		id, _ := strconv.ParseInt(string(payload.GetStringBytes("sub")), 10, 64)
		if id < 1 {
			return 0, "", errors.New("invalid token payload")
		}

		username := string(payload.GetStringBytes("username"))
		if username == "" {
			return 0, "", errors.New("invalid token payload")
		}

		return id, username, nil
	}

	return func(ctx *fasthttp.RequestCtx) {
		id, username, err := verify(ctx.Request.Header.Peek("Authorization"))
		if err != nil {
			erred(ctx, fasthttp.StatusUnauthorized, err)
		} else {
			ctx.SetUserValue("sub", id)
			ctx.SetUserValue("username", username)

			next(ctx)
		}
	}
}
