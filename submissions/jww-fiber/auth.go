package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"hash"
	"io"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gofiber/fiber/v3"
)

type authUser struct {
	ID       int64
	Username string
}

type tokenVerifier struct {
	pool sync.Pool
}

// base64url of {"alg":"HS256","typ":"JWT"}. Tokens minted with the standard
// header take the fast path and skip decoding the header JSON.
const hdrHS256 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"

func newTokenVerifier(secret []byte) *tokenVerifier {
	v := &tokenVerifier{}
	v.pool.New = func() any { return hmac.New(sha256.New, secret) }
	return v
}

// verifyToken checks the compact JWT structure, the HS256 signature and the
// exp claim. Every call verifies the signature; nothing is cached.
func (v *tokenVerifier) verifyToken(tok string) (claims, bool) {
	var cl claims

	i := strings.IndexByte(tok, '.')
	j := strings.LastIndexByte(tok, '.')
	if i <= 0 || j <= i+1 || j == len(tok)-1 {
		return cl, false
	}
	if strings.IndexByte(tok[i+1:j], '.') >= 0 {
		return cl, false
	}

	sig, err := base64.RawURLEncoding.DecodeString(tok[j+1:])
	if err != nil || len(sig) != sha256.Size {
		return cl, false
	}

	m := v.pool.Get().(hash.Hash)
	m.Reset()
	_, _ = io.WriteString(m, tok[:j])
	var sum [sha256.Size]byte
	want := m.Sum(sum[:0])
	v.pool.Put(m)
	if !hmac.Equal(sig, want) {
		return cl, false
	}

	if tok[:i] != hdrHS256 {
		hb, err := base64.RawURLEncoding.DecodeString(tok[:i])
		if err != nil {
			return cl, false
		}
		var hdr struct {
			Alg string `json:"alg"`
		}
		if json.Unmarshal(hb, &hdr) != nil || hdr.Alg != "HS256" {
			return cl, false
		}
	}
	pb, err := base64.RawURLEncoding.DecodeString(tok[i+1 : j])
	if err != nil || json.Unmarshal(pb, &cl) != nil {
		return cl, false
	}
	if cl.Exp <= float64(time.Now().Unix()) {
		return cl, false
	}
	return cl, true
}

// requireUser validates the Authorization header and token payload, writing the
// appropriate 401 response and returning ok=false on failure.
func (v *tokenVerifier) requireUser(c fiber.Ctx) (authUser, bool) {
	h := c.Get(fiber.HeaderAuthorization)
	if !strings.HasPrefix(h, "Bearer ") {
		_ = writeRaw(c, fiber.StatusUnauthorized, errMissingBearer)
		return authUser{}, false
	}

	cl, ok := v.verifyToken(h[len("Bearer "):])
	if !ok {
		_ = writeRaw(c, fiber.StatusUnauthorized, errInvalidToken)
		return authUser{}, false
	}

	uid, ok := parseSub(cl.Sub)
	if !ok {
		_ = writeRaw(c, fiber.StatusUnauthorized, errInvalidPayload)
		return authUser{}, false
	}
	name, ok := parseUsername(cl.Username)
	if !ok {
		_ = writeRaw(c, fiber.StatusUnauthorized, errInvalidPayload)
		return authUser{}, false
	}

	return authUser{ID: uid, Username: name}, true
}

func parseSub(raw json.RawMessage) (int64, bool) {
	if len(raw) == 0 || raw[0] != '"' {
		return 0, false
	}
	var s string
	if json.Unmarshal(raw, &s) != nil {
		return 0, false
	}
	id, err := strconv.ParseInt(s, 10, 64)
	return id, err == nil && id > 0
}

func parseUsername(raw json.RawMessage) (string, bool) {
	if len(raw) == 0 || raw[0] != '"' {
		return "", false
	}
	var s string
	if json.Unmarshal(raw, &s) != nil {
		return "", false
	}
	return s, true
}
