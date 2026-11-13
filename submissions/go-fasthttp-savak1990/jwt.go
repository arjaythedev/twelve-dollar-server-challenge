package main

import (
	"bytes"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"hash"
	"sync"
)

type jwtError uint8

const (
	jwtOK      jwtError = iota
	jwtInvalid          // malformed, bad signature, not HS256, expired → "invalid or expired token"
	jwtPayload          // valid token, bad sub/username → "invalid token payload"
)

func (e jwtError) String() string { return [...]string{"ok", "invalid", "payload"}[e] }

// hmacKey holds the precomputed HMAC-SHA256 inner/outer pads, so verifying a token is two SHA-256
// runs with no per-request key setup.
type hmacKey struct{ ipad, opad [sha256.BlockSize]byte }

func newHMACKey(secret []byte) *hmacKey {
	k := &hmacKey{}
	if len(secret) > sha256.BlockSize {
		s := sha256.Sum256(secret)
		secret = s[:]
	}
	for i := range k.ipad {
		k.ipad[i], k.opad[i] = 0x36, 0x5c
	}
	for i, c := range secret {
		k.ipad[i] ^= c
		k.opad[i] ^= c
	}
	return k
}

var sha256Pool = sync.Pool{New: func() any { return sha256.New() }}

// mac appends HMAC-SHA256(key, msg) to dst.
func (k *hmacKey) mac(dst, msg []byte) []byte {
	h := sha256Pool.Get().(hash.Hash)
	h.Reset()
	h.Write(k.ipad[:])
	h.Write(msg)
	mark := len(dst)
	dst = h.Sum(dst) // inner digest lands at dst[mark:], then is overwritten by the outer one
	h.Reset()
	h.Write(k.opad[:])
	h.Write(dst[mark:])
	dst = h.Sum(dst[:mark])
	sha256Pool.Put(h)
	return dst
}

// b64url appends the base64url decoding of src (padding optional) to dst.
func b64url(dst, src []byte) ([]byte, bool) {
	for len(src) > 0 && src[len(src)-1] == '=' {
		src = src[:len(src)-1]
	}
	n := base64.RawURLEncoding.DecodedLen(len(src))
	start := len(dst)
	dst = grow(dst, n)
	w, err := base64.RawURLEncoding.Decode(dst[start:start+n], src)
	return dst[:start+w], err == nil
}

func grow(b []byte, n int) []byte {
	if cap(b)-len(b) < n {
		nb := make([]byte, len(b), 2*cap(b)+n)
		copy(nb, b)
		b = nb
	}
	return b[:len(b)+n]
}

// verifyJWT checks an HS256 compact JWT and returns the user id (sub) and username. Slices it
// returns point into *scratch, which it resets and reuses.
func verifyJWT(token []byte, key *hmacKey, scratch *[]byte, now int64) (int64, []byte, jwtError) {
	d1 := bytes.IndexByte(token, '.')
	if d1 < 0 {
		return 0, nil, jwtInvalid
	}
	d2 := bytes.IndexByte(token[d1+1:], '.')
	if d2 < 0 {
		return 0, nil, jwtInvalid
	}
	d2 += d1 + 1
	if bytes.IndexByte(token[d2+1:], '.') >= 0 {
		return 0, nil, jwtInvalid
	}
	buf := (*scratch)[:0]

	// signature first: it is the cheapest way to reject junk before parsing anything
	var ok bool
	sigStart := len(buf)
	if buf, ok = b64url(buf, token[d2+1:]); !ok || len(buf)-sigStart != sha256.Size {
		return 0, nil, jwtInvalid
	}
	macStart := len(buf)
	buf = key.mac(buf, token[:d2])
	if subtle.ConstantTimeCompare(buf[sigStart:macStart], buf[macStart:]) != 1 {
		return 0, nil, jwtInvalid
	}

	hdrStart := len(buf)
	if buf, ok = b64url(buf, token[:d1]); !ok {
		return 0, nil, jwtInvalid
	}
	hs256 := false
	if !parseObject(buf[hdrStart:], func(k, v []byte, kind byte) {
		if string(k) == "alg" {
			hs256 = kind == kindString && string(v) == "HS256"
		}
	}) || !hs256 {
		return 0, nil, jwtInvalid
	}

	plStart := len(buf)
	if buf, ok = b64url(buf, token[d1+1:d2]); !ok {
		return 0, nil, jwtInvalid
	}
	var (
		exp       int64
		hasExp    bool
		sub       int64
		subOK     bool
		user      []byte
		userOK    bool
		userRaw   bool
		userStart int
	)
	payload := buf[plStart:]
	if !parseObject(payload, func(k, v []byte, kind byte) {
		switch string(k) {
		case "exp":
			if kind == kindNumber {
				exp, hasExp = parseNum(v)
			}
		case "sub":
			if kind == kindString {
				sub, subOK = parseID(v)
			}
		case "username":
			userOK = kind == kindString
			user, userRaw = v, bytes.IndexByte(v, '\\') >= 0
		}
	}) {
		return 0, nil, jwtInvalid
	}
	if !hasExp || exp <= now {
		return 0, nil, jwtInvalid
	}
	if !subOK || !userOK {
		return 0, nil, jwtPayload
	}
	if userRaw {
		userStart = len(buf)
		if buf, ok = unescape(buf, user); !ok {
			return 0, nil, jwtInvalid
		}
		user = buf[userStart:]
	}
	*scratch = buf
	return sub, user, jwtOK
}

// parseNum reads a JSON integer (fractional/exponent forms are truncated at the first non-digit).
func parseNum(b []byte) (int64, bool) {
	if len(b) == 0 {
		return 0, false
	}
	neg := b[0] == '-'
	if neg {
		b = b[1:]
	}
	var n int64
	i := 0
	for ; i < len(b) && b[i] >= '0' && b[i] <= '9'; i++ {
		n = n*10 + int64(b[i]-'0')
	}
	if i == 0 {
		return 0, false
	}
	if neg {
		n = -n
	}
	return n, true
}
