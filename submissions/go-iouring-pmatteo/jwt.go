package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
)

type authResult uint8

const (
	authOK         authResult = iota
	authInvalid               // bad signature, wrong alg, expired, malformed: "invalid or expired token"
	authBadPayload            // valid signature, unusable claims: "invalid token payload"
)

// verifier verifies HS256 tokens. Nothing about a token is remembered between requests (rule 5):
// every signature is recomputed. The buffers are reused, the hasher is reset, so a verification
// allocates nothing.
type verifier struct {
	mac     hash.Hash
	header  []byte
	payload []byte
	sig     []byte
	sum     []byte
	scratch []byte
}

func newVerifier(secret []byte) *verifier {
	return &verifier{
		mac:     hmac.New(sha256.New, secret),
		header:  make([]byte, 0, 256),
		payload: make([]byte, 0, 1024),
		sig:     make([]byte, 0, 64),
		sum:     make([]byte, 0, sha256.Size),
		scratch: make([]byte, 0, 256),
	}
}

type claims struct {
	userID   int64
	username []byte // a view into v.scratch, valid until the next verify
}

// verify checks the token and extracts the claims. The checks run in the order test.sh asserts:
// alg, signature and exp first (authInvalid), then the claim shapes (authBadPayload).
func (v *verifier) verify(token []byte, nowUnix int64) (claims, authResult) {
	var c claims

	dot1 := bytes.IndexByte(token, '.')
	if dot1 < 0 {
		return c, authInvalid
	}
	dot2 := bytes.IndexByte(token[dot1+1:], '.')
	if dot2 < 0 {
		return c, authInvalid
	}
	dot2 += dot1 + 1
	if bytes.IndexByte(token[dot2+1:], '.') >= 0 {
		return c, authInvalid
	}

	var ok bool
	if v.header, ok = decodeB64(v.header[:0], token[:dot1]); !ok {
		return c, authInvalid
	}
	alg, kind := jsonField(v.header, "alg")
	if kind != jsonString || string(alg) != "HS256" {
		return c, authInvalid
	}

	if v.sig, ok = decodeB64(v.sig[:0], token[dot2+1:]); !ok {
		return c, authInvalid
	}
	v.mac.Reset()
	v.mac.Write(token[:dot2])
	v.sum = v.mac.Sum(v.sum[:0])
	if !hmac.Equal(v.sum, v.sig) {
		return c, authInvalid
	}

	if v.payload, ok = decodeB64(v.payload[:0], token[dot1+1:dot2]); !ok {
		return c, authInvalid
	}

	// exp is mandatory: a token without one is rejected outright.
	expRaw, kind := jsonField(v.payload, "exp")
	if kind != jsonNumber {
		return c, authInvalid
	}
	exp, ok := parseSeconds(expRaw)
	if !ok || exp <= nowUnix {
		return c, authInvalid
	}

	subRaw, kind := jsonField(v.payload, "sub")
	if kind != jsonString {
		return c, authBadPayload
	}
	id, ok := parsePositiveInt(subRaw)
	if !ok {
		return c, authBadPayload
	}

	nameRaw, kind := jsonField(v.payload, "username")
	if kind != jsonString {
		return c, authBadPayload
	}
	name, ok := jsonUnescape(v.scratch[:0], nameRaw)
	if !ok {
		return c, authBadPayload
	}
	v.scratch = name

	c.userID = id
	c.username = name
	return c, authOK
}

// decodeB64 decodes unpadded base64url. Tokens in the wild sometimes carry padding, so both are
// accepted.
func decodeB64(dst, src []byte) ([]byte, bool) {
	for len(src) > 0 && src[len(src)-1] == '=' {
		src = src[:len(src)-1]
	}
	need := base64.RawURLEncoding.DecodedLen(len(src))
	if cap(dst) < need {
		dst = make([]byte, 0, need*2)
	}
	dst = dst[:need]
	n, err := base64.RawURLEncoding.Decode(dst, src)
	if err != nil {
		return dst[:0], false
	}
	return dst[:n], true
}

// parseSeconds reads an integer-or-fractional epoch value, truncating any fraction.
func parseSeconds(b []byte) (int64, bool) {
	if i := bytes.IndexByte(b, '.'); i >= 0 {
		b = b[:i]
	}
	neg := false
	if len(b) > 0 && b[0] == '-' {
		neg, b = true, b[1:]
	}
	if len(b) == 0 || len(b) > 19 {
		return 0, false
	}
	var v int64
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	if neg {
		v = -v
	}
	return v, true
}

// parsePositiveInt accepts the decimal digits of a positive integer and nothing else: no sign,
// no leading zero, no whitespace. This is the rule for a JWT "sub" and for a ":id" path segment.
func parsePositiveInt(b []byte) (int64, bool) {
	if len(b) == 0 || len(b) > 18 || b[0] == '0' {
		return 0, false
	}
	var v int64
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	return v, true
}
