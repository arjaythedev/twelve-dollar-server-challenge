package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
)

var jwtSecret []byte

// authenticate verifies an HS256 bearer token on every call and returns the user id and
// name, or the spec's 401 message.
func authenticate(header []byte, now int64) (int64, string, string) {
	token, ok := bytes.CutPrefix(header, []byte("Bearer "))
	if !ok {
		return 0, "", "missing bearer token"
	}
	const invalid = "invalid or expired token"
	h64, rest, ok1 := bytes.Cut(token, []byte("."))
	p64, s64, ok2 := bytes.Cut(rest, []byte("."))
	if !ok1 || !ok2 {
		return 0, "", invalid
	}
	var sig [sha256.Size]byte
	if base64.RawURLEncoding.DecodedLen(len(s64)) != len(sig) {
		return 0, "", invalid
	}
	if n, err := base64.RawURLEncoding.Decode(sig[:], s64); err != nil || n != len(sig) {
		return 0, "", invalid
	}
	mac := hmac.New(sha256.New, jwtSecret)
	mac.Write(token[:len(h64)+1+len(p64)])
	if !hmac.Equal(mac.Sum(nil), sig[:]) {
		return 0, "", invalid
	}
	var head, claims map[string]any
	if decodeJSON(h64, &head) != nil || head["alg"] != "HS256" || decodeJSON(p64, &claims) != nil {
		return 0, "", invalid
	}
	if exp, present := claims["exp"]; present {
		if v, ok := exp.(float64); !ok || float64(now) >= v {
			return 0, "", invalid
		}
	}
	if nbf, present := claims["nbf"]; present {
		if v, ok := nbf.(float64); !ok || float64(now) < v {
			return 0, "", invalid
		}
	}
	sub, _ := claims["sub"].(string)
	name, nameOK := claims["username"].(string)
	id, idOK := parseID([]byte(sub))
	if !idOK || !nameOK {
		return 0, "", "invalid token payload"
	}
	return id, name, ""
}

func decodeJSON(b64 []byte, v any) error {
	raw, err := base64.RawURLEncoding.AppendDecode(nil, b64)
	if err != nil {
		return err
	}
	return json.Unmarshal(raw, v)
}
