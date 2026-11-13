package main

import (
	"bytes"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"hash"
	"sync"
	"time"

	"github.com/valyala/fasthttp"
)

var (
	kIPad [64]byte
	kOPad [64]byte

	sha256Pool = sync.Pool{New: func() any { return sha256.New() }}

	// jwtScratchPool hands out the buffer that decoded JWT pieces are written
	// into. One buffer is reused sequentially for header, signature and payload,
	// so a verification allocates nothing.
	jwtScratchPool = sync.Pool{New: func() any {
		b := make([]byte, 512)
		return &b
	}}
)

// initHMAC precomputes the HMAC-SHA256 key pads so a verification only needs two
// SHA-256 passes over reused hashers.
func initHMAC(secret []byte) {
	var k [64]byte
	if len(secret) > 64 {
		h := sha256.Sum256(secret)
		copy(k[:], h[:])
	} else {
		copy(k[:], secret)
	}
	for i := range k {
		kIPad[i] = k[i] ^ 0x36
		kOPad[i] = k[i] ^ 0x5c
	}
}

// hmacMatches computes HMAC-SHA256(key, msg) and compares it to sig in constant
// time. It allocates nothing: the hash and both digests live on the stack.
func hmacMatches(msg, sig []byte) bool {
	h := sha256Pool.Get().(hash.Hash)
	var inner [sha256.Size]byte
	var out [sha256.Size]byte
	h.Reset()
	h.Write(kIPad[:])
	h.Write(msg)
	ib := h.Sum(inner[:0])
	h.Reset()
	h.Write(kOPad[:])
	h.Write(ib)
	ob := h.Sum(out[:0])
	sha256Pool.Put(h)
	return subtle.ConstantTimeCompare(ob, sig) == 1
}

func acquireScratch() *[]byte  { return jwtScratchPool.Get().(*[]byte) }
func releaseScratch(b *[]byte) { jwtScratchPool.Put(b) }

// decodeInto base64url-decodes src into buf, growing buf only if needed.
func decodeInto(buf *[]byte, src []byte) ([]byte, bool) {
	n := base64.RawURLEncoding.DecodedLen(len(src))
	if cap(*buf) < n {
		*buf = make([]byte, n)
	} else {
		*buf = (*buf)[:n]
	}
	m, err := base64.RawURLEncoding.Decode(*buf, src)
	if err != nil {
		return nil, false
	}
	return (*buf)[:m], true
}

func authFail(ctx *fasthttp.RequestCtx, body []byte) (int64, []byte, bool) {
	respond(ctx, 401, body)
	return 0, nil, false
}

// authenticate verifies the bearer JWT on every request (rule 5: never remember a
// verification) and returns the user id and username. The returned username
// aliases sc, so the caller must keep sc until it is done with it.
func authenticate(ctx *fasthttp.RequestCtx, sc *[]byte) (int64, []byte, bool) {
	auth := ctx.Request.Header.Peek("Authorization")
	if len(auth) < 7 || !bytes.HasPrefix(auth, bearerPrefix) {
		return authFail(ctx, errMissingBearer)
	}
	token := auth[7:]
	i1 := bytes.IndexByte(token, '.')
	if i1 < 0 {
		return authFail(ctx, errBadToken)
	}
	i2 := bytes.LastIndexByte(token, '.')
	if i2 <= i1 {
		return authFail(ctx, errBadToken)
	}
	headB64 := token[:i1]
	payloadB64 := token[i1+1 : i2]
	sigB64 := token[i2+1:]

	hb, ok := decodeInto(sc, headB64)
	if !ok {
		return authFail(ctx, errBadToken)
	}
	alg, ok := headerAlg(hb)
	if !ok || !bytes.Equal(alg, valHS256) {
		return authFail(ctx, errBadToken)
	}

	sig, ok := decodeInto(sc, sigB64)
	if !ok {
		return authFail(ctx, errBadToken)
	}
	if !hmacMatches(token[:i2], sig) {
		return authFail(ctx, errBadToken)
	}

	pb, ok := decodeInto(sc, payloadB64)
	if !ok {
		return authFail(ctx, errBadToken)
	}
	var f jwtFields
	scanPayload(pb, &f)
	if f.bad || f.expBad || f.nbfBad {
		return authFail(ctx, errBadToken)
	}
	now := float64(time.Now().Unix())
	if f.hasExp && now >= f.exp {
		return authFail(ctx, errBadToken)
	}
	if f.hasNbf && now < f.nbf {
		return authFail(ctx, errBadToken)
	}
	if !f.hasSub || !f.subStr || !f.hasUser || !f.userStr {
		return authFail(ctx, errBadPayload)
	}
	id, ok2, overflow := parseID(f.sub)
	if !ok2 || overflow {
		return authFail(ctx, errBadPayload)
	}
	return id, f.username, true
}
