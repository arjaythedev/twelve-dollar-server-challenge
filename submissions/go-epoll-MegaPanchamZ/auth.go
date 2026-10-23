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
	authOK authResult = iota
	authMissing
	authInvalid
	authPayload
)

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

var (
	bearerPrefix = []byte("Bearer ")
	// Standard base64url of {"alg":"HS256","typ":"JWT"}
	stdHS256Hdr = []byte("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9")
)

func (v *verifier) verify(header []byte, haveHeader bool, nowSec int64) (int64, []byte, authResult) {
	if !haveHeader || !bytes.HasPrefix(header, bearerPrefix) {
		return 0, nil, authMissing
	}
	tok := header[len(bearerPrefix):]
	d1 := 36
	stdHdr := false
	if len(tok) > 80 && tok[36] == '.' && string(tok[:36]) == "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9" {
		stdHdr = true
	} else {
		d1 = bytes.IndexByte(tok, '.')
		if d1 <= 0 {
			return 0, nil, authInvalid
		}
	}
	d2 := bytes.IndexByte(tok[d1+1:], '.')
	if d2 <= 0 {
		return 0, nil, authInvalid
	}
	d2 += d1 + 1

	sigB64 := tok[d2+1:]
	if len(sigB64) != 43 {
		return 0, nil, authInvalid
	}
	v.mac.Reset()
	v.mac.Write(tok[:d2])
	base64.RawURLEncoding.Encode(v.sig[:43], v.mac.Sum(v.sum[:0]))
	if !hmac.Equal(sigB64, v.sig[:43]) {
		return 0, nil, authInvalid
	}

	if !stdHdr {
		head, ok := v.decodeSegment(tok[:d1])
		if !ok || !v.headerIsHS256(head) {
			return 0, nil, authInvalid
		}
	}

	payload, ok := v.decodeSegment(tok[d1+1 : d2])
	if !ok {
		return 0, nil, authInvalid
	}
	return v.claims(payload, float64(nowSec))
}

func (v *verifier) decodeSegment(s []byte) ([]byte, bool) {
	n := base64.RawURLEncoding.DecodedLen(len(s))
	if n > cap(v.seg) {
		return nil, false
	}
	out := v.seg[:n]
	if _, err := base64.RawURLEncoding.Decode(out, s); err != nil {
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
			alg = m.val[0] == '"' && string(unescapeRef(v.scratch[:0], m.val)) == "HS256"
		}
	}
	return !w.err && alg
}

func (v *verifier) claims(payload []byte, now float64) (int64, []byte, authResult) {
	w, ok := newObjectWalker(payload)
	if !ok {
		if !validJSON(payload) {
			return 0, nil, authInvalid
		}
		return 0, nil, authPayload
	}
	var sub int64
	var user []byte
	subOK, userOK, expSeen := false, false, false
	for m, more := w.next(); more; m, more = w.next() {
		switch string(m.key) {
		case `"sub"`:
			subOK = false
			if m.val[0] == '"' {
				sub, subOK = parsePositiveID(unescapeRef(v.scratch[:0], m.val))
			}
		case `"username"`:
			userOK = m.val[0] == '"'
			if userOK {
				user = unescapeRef(v.user[:0], m.val)
			}
		case `"exp"`:
			exp, isNum := parseJSONNumber(m.val)
			if !isNum || now >= exp {
				return 0, nil, authInvalid
			}
			expSeen = true
		case `"nbf"`:
			nbf, isNum := parseJSONNumber(m.val)
			if !isNum || now < nbf {
				return 0, nil, authInvalid
			}
		default:
			if bytes.IndexByte(m.key, '\\') >= 0 {
				switch string(unescape(v.scratch[:0], m.key)) {
				case "sub":
					subOK = false
					if m.val[0] == '"' {
						sub, subOK = parsePositiveID(unescapeRef(v.scratch[:0], m.val))
					}
				case "username":
					userOK = m.val[0] == '"'
					if userOK {
						user = unescapeRef(v.user[:0], m.val)
					}
				case "exp":
					exp, isNum := parseJSONNumber(m.val)
					if !isNum || now >= exp {
						return 0, nil, authInvalid
					}
					expSeen = true
				case "nbf":
					nbf, isNum := parseJSONNumber(m.val)
					if !isNum || now < nbf {
						return 0, nil, authInvalid
					}
				}
			}
		}
	}
	if w.err || !expSeen {
		return 0, nil, authInvalid
	}
	if !subOK || !userOK {
		return 0, nil, authPayload
	}
	return sub, user, authOK
}

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
