package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"hash"
	"math"
	"time"
)

// HS256 verification, done in full on every request (rule 5). The keyed HMAC state is reused
// through Reset, which saves re-deriving the key pads but remembers nothing about tokens.
type verifier struct {
	mac hash.Hash
	sum [sha256.Size]byte
	enc [64]byte
	buf [1024]byte
}

func newVerifier(secret string) *verifier {
	return &verifier{mac: hmac.New(sha256.New, []byte(secret))}
}

var (
	bearer       = []byte("Bearer ")
	standardHead = []byte(`{"alg":"HS256","typ":"JWT"}`)
)

// auth returns the user id and username, or the error body to send with a 401.
func (v *verifier) auth(header []byte, hasHeader bool) (int64, string, []byte) {
	if !hasHeader || !bytes.HasPrefix(header, bearer) {
		return 0, "", errMissingToken
	}
	tok := header[len(bearer):]
	i := bytes.IndexByte(tok, '.')
	if i < 0 {
		return 0, "", errInvalidToken
	}
	j := bytes.IndexByte(tok[i+1:], '.')
	if j < 0 {
		return 0, "", errInvalidToken
	}
	j += i + 1
	headB64, payloadB64, sig := tok[:i], tok[i+1:j], tok[j+1:]
	if bytes.IndexByte(sig, '.') >= 0 {
		return 0, "", errInvalidToken
	}

	head, ok := b64(v.buf[:], headB64)
	if !ok || !(bytes.Equal(head, standardHead) || algIsHS256(head)) {
		return 0, "", errInvalidToken
	}

	v.mac.Reset()
	v.mac.Write(tok[:j])
	sum := v.mac.Sum(v.sum[:0])
	n := base64.RawURLEncoding.EncodedLen(len(sum))
	base64.RawURLEncoding.Encode(v.enc[:n], sum)
	if subtle.ConstantTimeCompare(v.enc[:n], sig) != 1 {
		return 0, "", errInvalidToken
	}

	raw, ok := b64(v.buf[:], payloadB64)
	if !ok {
		return 0, "", errInvalidToken
	}
	var payload map[string]any
	if json.Unmarshal(raw, &payload) != nil || payload == nil {
		return 0, "", errInvalidToken
	}
	now := float64(time.Now().Unix())
	if exp, present := payload["exp"]; present && exp != nil {
		e, isNum := exp.(float64)
		if !isNum || now >= e {
			return 0, "", errInvalidToken
		}
	}
	if nbf, present := payload["nbf"]; present && nbf != nil {
		f, isNum := nbf.(float64)
		if !isNum || now < f {
			return 0, "", errInvalidToken
		}
	}
	sub, _ := payload["sub"].(string)
	id, valid, tooBig := parseID([]byte(sub))
	name, isString := payload["username"].(string)
	if !valid || tooBig || !isString {
		return 0, "", errInvalidPayload
	}
	return id, name, nil
}

func algIsHS256(head []byte) bool {
	var h map[string]any
	if json.Unmarshal(head, &h) != nil {
		return false
	}
	alg, _ := h["alg"].(string)
	return alg == "HS256"
}

// b64 decodes unpadded base64url into dst, or into a new slice if dst is too small.
func b64(dst []byte, src []byte) ([]byte, bool) {
	n := base64.RawURLEncoding.DecodedLen(len(src))
	if n > len(dst) {
		dst = make([]byte, n)
	}
	n, err := base64.RawURLEncoding.Decode(dst, src)
	return dst[:n], err == nil
}

// parseID accepts plain decimal digits with a value > 0. tooBig marks ids past SQLite's
// INTEGER range: they are valid ids, but no such post can exist.
func parseID(s []byte) (id int64, valid, tooBig bool) {
	if len(s) == 0 {
		return 0, false, false
	}
	var v uint64
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false, false
		}
		if v > (math.MaxUint64-9)/10 {
			tooBig = true
			continue
		}
		v = v*10 + uint64(c-'0')
	}
	if tooBig || v > math.MaxInt64 {
		return 0, true, true
	}
	return int64(v), v > 0, false
}
