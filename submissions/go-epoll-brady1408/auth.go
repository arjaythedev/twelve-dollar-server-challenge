package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"hash"
	"strconv"
	"strings"
)

type authStatus uint8

const (
	authOK authStatus = iota
	authMissing
	authInvalid
	authBadPayload
)

type claims struct {
	userID   int64
	username string
}

// verifier checks HS256 bearer tokens. Nothing is cached between calls; the
// buffers are reused only to avoid allocation. Single-goroutine use.
type verifier struct {
	mac     hash.Hash
	sum     []byte
	decoded []byte
}

func newVerifier(secret []byte) *verifier {
	return &verifier{mac: hmac.New(sha256.New, secret), sum: make([]byte, 0, 32), decoded: make([]byte, 0, 1024)}
}

var bearerPrefix = []byte("Bearer ")

type jwtHeader struct {
	Alg string `json:"alg"`
}

type jwtPayload struct {
	Sub      json.RawMessage `json:"sub"`
	Username json.RawMessage `json:"username"`
	Exp      json.RawMessage `json:"exp"`
}

func (v *verifier) authenticate(authHeader []byte, now int64) (claims, authStatus) {
	if !bytes.HasPrefix(authHeader, bearerPrefix) {
		return claims{}, authMissing
	}
	token := bytes.TrimSpace(authHeader[len(bearerPrefix):])
	dot1 := bytes.IndexByte(token, '.')
	if dot1 < 0 {
		return claims{}, authInvalid
	}
	dot2 := bytes.IndexByte(token[dot1+1:], '.')
	if dot2 < 0 {
		return claims{}, authInvalid
	}
	dot2 += dot1 + 1
	if bytes.IndexByte(token[dot2+1:], '.') >= 0 {
		return claims{}, authInvalid
	}
	headerB64, payloadB64, sigB64 := token[:dot1], token[dot1+1:dot2], token[dot2+1:]

	// Header: must be HS256. Fully consumed before decode() reuses the scratch buffer.
	hdrBytes, ok := v.decode(headerB64)
	if !ok {
		return claims{}, authInvalid
	}
	var hdr jwtHeader
	if json.Unmarshal(hdrBytes, &hdr) != nil || hdr.Alg != "HS256" {
		return claims{}, authInvalid
	}

	// Signature over "header.payload".
	sig, ok := v.decodeSig(sigB64)
	if !ok {
		return claims{}, authInvalid
	}
	v.mac.Reset()
	v.mac.Write(token[:dot2])
	v.sum = v.mac.Sum(v.sum[:0])
	if !hmac.Equal(v.sum, sig) {
		return claims{}, authInvalid
	}

	// Payload: exp first (invalid/expired), then sub and username (payload).
	payloadBytes, ok := v.decode(payloadB64)
	if !ok {
		return claims{}, authInvalid
	}
	var p jwtPayload
	if json.Unmarshal(payloadBytes, &p) != nil {
		return claims{}, authInvalid
	}
	exp, err := strconv.ParseFloat(string(p.Exp), 64)
	if err != nil || exp <= float64(now) {
		return claims{}, authInvalid
	}
	id, ok := parseSub(p.Sub)
	if !ok {
		return claims{}, authBadPayload
	}
	if len(p.Username) == 0 || p.Username[0] != '"' {
		return claims{}, authBadPayload
	}
	var name string
	if json.Unmarshal(p.Username, &name) != nil {
		return claims{}, authBadPayload
	}
	return claims{userID: id, username: name}, authOK
}

// parseSub accepts a positive integer as a JSON string ("42") or number (42).
func parseSub(raw json.RawMessage) (int64, bool) {
	if len(raw) == 0 {
		return 0, false
	}
	if raw[0] == '"' {
		var s string
		if json.Unmarshal(raw, &s) != nil {
			return 0, false
		}
		id, err := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
		return id, err == nil && id > 0
	}
	f, err := strconv.ParseFloat(string(raw), 64)
	if err != nil || f <= 0 || f != float64(int64(f)) || f > 1<<53 {
		return 0, false
	}
	return int64(f), true
}

// decode base64url-decodes into the reusable scratch buffer.
func (v *verifier) decode(src []byte) ([]byte, bool) {
	n := base64.RawURLEncoding.DecodedLen(len(src))
	if cap(v.decoded) < n {
		v.decoded = make([]byte, 0, n)
	}
	buf := v.decoded[:n]
	m, err := base64.RawURLEncoding.Decode(buf, src)
	if err != nil {
		return nil, false
	}
	return buf[:m], true
}

// decodeSig decodes the signature and requires exactly 32 bytes (SHA-256).
func (v *verifier) decodeSig(src []byte) ([]byte, bool) {
	var buf [48]byte
	if base64.RawURLEncoding.DecodedLen(len(src)) > len(buf) {
		return nil, false
	}
	m, err := base64.RawURLEncoding.Decode(buf[:], src)
	if err != nil || m != sha256.Size {
		return nil, false
	}
	return buf[:m], true
}
