package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
	"strconv"
)

const (
	authOK = iota
	authMissing
	authInvalid
	authPayload
)

// A verifier reuses standard-library HMAC state and decoding workspace, never
// a token or a verification result. Each call authenticates its complete input.
type verifier struct {
	signer       hash.Hash
	decoded      []byte
	stringBuffer []byte
	digest       [sha256.Size]byte
}

func newVerifier(key []byte) *verifier {
	return &verifier{
		signer:       hmac.New(sha256.New, key),
		decoded:      make([]byte, 0, 4096),
		stringBuffer: make([]byte, 0, 256),
	}
}

var jwtEncoding = base64.RawURLEncoding.Strict()

func decodeTokenPart(part, workspace []byte) ([]byte, bool) {
	// encoding/base64 accepts line breaks even in Strict mode; JWT segments do
	// not. Strict additionally rejects noncanonical unused trailing bits.
	if bytes.IndexAny(part, "\r\n") >= 0 {
		return nil, false
	}
	decoded, err := jwtEncoding.AppendDecode(workspace[:0], part)
	return decoded, err == nil
}

func (v *verifier) stringValue(value byteValue) []byte {
	if !value.escaped {
		return value.raw
	}
	v.stringBuffer = unescape(value.raw, v.stringBuffer[:0])
	return v.stringBuffer
}

func (v *verifier) algorithm(encoded []byte) bool {
	decoded, ok := decodeTokenPart(encoded, v.decoded)
	v.decoded = decoded
	if !ok {
		return false
	}
	var algorithm byteValue
	valid, object := walkObject(decoded, v.stringBuffer[:0], func(key []byte, value byteValue) {
		if string(key) == "alg" {
			algorithm = value
		}
	})
	return valid && object && algorithm.kind == byteString && string(v.stringValue(algorithm)) == "HS256"
}

func (v *verifier) claims(encoded []byte, now int64) (int, int64, []byte) {
	decoded, ok := decodeTokenPart(encoded, v.decoded)
	v.decoded = decoded
	if !ok {
		return authInvalid, 0, nil
	}
	var subject, username, expiry byteValue
	valid, object := walkObject(decoded, v.stringBuffer[:0], func(key []byte, value byteValue) {
		switch string(key) {
		case "sub":
			subject = value
		case "username":
			username = value
		case "exp":
			expiry = value
		}
	})
	if !valid || !object || expiry.kind != byteNumber {
		return authInvalid, 0, nil
	}
	expires, err := strconv.ParseFloat(string(expiry.raw), 64)
	if err != nil || expires <= float64(now) {
		return authInvalid, 0, nil
	}
	if subject.kind != byteString || username.kind != byteString {
		return authPayload, 0, nil
	}
	id, valid := positive(v.stringValue(subject))
	if !valid {
		return authPayload, 0, nil
	}
	// The name is borrowed until the next verify call, just like decoded claims.
	return authOK, id, v.stringValue(username)
}

func (v *verifier) verify(authorization []byte, present bool, now int64) (int, int64, []byte) {
	if !present || !bytes.HasPrefix(authorization, []byte("Bearer ")) {
		return authMissing, 0, nil
	}
	token := authorization[len("Bearer "):]
	lastDot := bytes.LastIndexByte(token, '.')
	if lastDot < 0 {
		return authInvalid, 0, nil
	}
	signed, signature := token[:lastDot], token[lastDot+1:]
	header, payload, found := bytes.Cut(signed, []byte{'.'})
	if !found || bytes.IndexByte(payload, '.') >= 0 {
		return authInvalid, 0, nil
	}
	given, valid := decodeTokenPart(signature, v.decoded)
	v.decoded = given
	if !valid || len(given) != sha256.Size {
		return authInvalid, 0, nil
	}
	v.signer.Reset()
	v.signer.Write(signed)
	expected := v.signer.Sum(v.digest[:0])
	if !hmac.Equal(expected, given) {
		return authInvalid, 0, nil
	}
	if !v.algorithm(header) {
		return authInvalid, 0, nil
	}
	return v.claims(payload, now)
}

// Identifier validation is shared by routes and the JWT subject claim.
func positive(raw []byte) (int64, bool) {
	if len(raw) == 0 || len(raw) > 16 || raw[0] == '0' {
		return 0, false
	}
	id, err := strconv.ParseUint(string(raw), 10, 54)
	return int64(id), err == nil && id > 0 && id <= 1<<53
}
