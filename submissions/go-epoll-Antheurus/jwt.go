package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
	"time"
)

type authResult int

const (
	authOK authResult = iota
	authMissing
	authInvalid
	authPayload
)

// verifier checks an HS256 JWT on every call. The HMAC keeps its keyed state between calls
// (that is the key schedule, not a remembered verification); signature, exp and nbf are
// checked from scratch for each token.
type verifier struct {
	mac     hash.Hash
	sum     [sha256.Size]byte
	sig     [64]byte
	seg     []byte
	scratch []byte
	user    []byte
}

func newVerifier(secret string) *verifier {
	return &verifier{
		mac:     hmac.New(sha256.New, []byte(secret)),
		seg:     make([]byte, 0, 16<<10),
		scratch: make([]byte, 0, 256),
		user:    make([]byte, 0, 1024),
	}
}

var bearer = []byte("Bearer ")

// verify returns the user id and username (valid until the next call) of a good token.
func (v *verifier) verify(header []byte, haveHeader bool) (int64, []byte, authResult) {
	if !haveHeader || !bytes.HasPrefix(header, bearer) {
		return 0, nil, authMissing
	}
	tok := header[len(bearer):]
	d1 := bytes.IndexByte(tok, '.')
	if d1 < 0 {
		return 0, nil, authInvalid
	}
	d2 := bytes.IndexByte(tok[d1+1:], '.')
	if d2 < 0 {
		return 0, nil, authInvalid
	}
	d2 += d1 + 1
	if bytes.IndexByte(tok[d2+1:], '.') >= 0 {
		return 0, nil, authInvalid
	}

	sigB64 := tok[d2+1:]
	if base64.RawURLEncoding.DecodedLen(len(sigB64)) != sha256.Size {
		return 0, nil, authInvalid
	}
	if _, err := base64.RawURLEncoding.Strict().Decode(v.sig[:], sigB64); err != nil {
		return 0, nil, authInvalid
	}
	v.mac.Reset()
	v.mac.Write(tok[:d2])
	if !hmac.Equal(v.mac.Sum(v.sum[:0]), v.sig[:sha256.Size]) {
		return 0, nil, authInvalid
	}

	head, ok := v.decodeSegment(tok[:d1])
	if !ok || !v.headerIsHS256(head) {
		return 0, nil, authInvalid
	}
	payload, ok := v.decodeSegment(tok[d1+1 : d2])
	if !ok {
		return 0, nil, authInvalid
	}
	return v.claims(payload)
}

func (v *verifier) decodeSegment(s []byte) ([]byte, bool) {
	n := base64.RawURLEncoding.DecodedLen(len(s))
	if n > cap(v.seg) {
		return nil, false
	}
	out := v.seg[:n]
	if _, err := base64.RawURLEncoding.Decode(out, s); err != nil || !validJSON(out) {
		return nil, false
	}
	return out, true
}

func (v *verifier) headerIsHS256(head []byte) bool {
	w, ok := newObjectWalker(head)
	if !ok {
		return false
	}
	alg := false
	for m, more := w.next(); more; m, more = w.next() {
		if keyIs(m.key, "alg", v.scratch) {
			alg = m.val[0] == '"' && string(unescape(v.scratch[:0], m.val)) == "HS256"
		}
	}
	return alg
}

func (v *verifier) claims(payload []byte) (int64, []byte, authResult) {
	w, ok := newObjectWalker(payload)
	if !ok {
		return 0, nil, authPayload
	}
	now := float64(time.Now().Unix())
	var sub int64
	subOK, userOK, expSeen := false, false, false
	for m, more := w.next(); more; m, more = w.next() {
		switch {
		case keyIs(m.key, "exp", v.scratch):
			exp, isNum := parseJSONNumber(m.val)
			if !isNum || now >= exp {
				return 0, nil, authInvalid
			}
			expSeen = true
		case keyIs(m.key, "nbf", v.scratch):
			nbf, isNum := parseJSONNumber(m.val)
			if !isNum || now < nbf {
				return 0, nil, authInvalid
			}
		case keyIs(m.key, "sub", v.scratch):
			subOK = false
			if m.val[0] == '"' {
				sub, subOK = parsePositiveID(unescape(v.scratch[:0], m.val))
			}
		case keyIs(m.key, "username", v.scratch):
			userOK = m.val[0] == '"'
			if userOK {
				v.user = unescape(v.user[:0], m.val)
			}
		}
	}
	if !expSeen {
		return 0, nil, authInvalid
	}
	if !subOK || !userOK {
		return 0, nil, authPayload
	}
	return sub, v.user, authOK
}

// parsePositiveID accepts 1 to 18 ASCII digits with a value above zero.
func parsePositiveID(b []byte) (int64, bool) {
	if len(b) == 0 || len(b) > 18 {
		return 0, false
	}
	var n int64
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int64(c-'0')
	}
	return n, n > 0
}
