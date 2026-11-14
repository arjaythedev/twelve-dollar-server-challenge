package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"testing"
)

const testSecret = "twelve-dollar-challenge"

func sign(header, payload string) string {
	h := base64.RawURLEncoding.EncodeToString([]byte(header))
	p := base64.RawURLEncoding.EncodeToString([]byte(payload))
	mac := hmac.New(sha256.New, []byte(testSecret))
	mac.Write([]byte(h + "." + p))
	return h + "." + p + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func TestAuthenticate(t *testing.T) {
	jwtSecret = []byte(testSecret)
	const now = 1_800_000_000
	hs256 := `{"alg":"HS256","typ":"JWT"}`
	valid := sign(hs256, `{"sub":"1","username":"golden_ember_1","iat":1799999000,"exp":1800003600}`)
	none := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"none","typ":"JWT"}`)) + "." +
		base64.RawURLEncoding.EncodeToString([]byte(`{"sub":"1","username":"u","exp":1800003600}`)) + "."
	tests := []struct {
		name, header string
		wantID       int64
		wantName     string
		wantMsg      string
	}{
		{"valid", "Bearer " + valid, 1, "golden_ember_1", ""},
		{"no header", "", 0, "", "missing bearer token"},
		{"basic", "Basic abc", 0, "", "missing bearer token"},
		{"garbage", "Bearer abc", 0, "", "invalid or expired token"},
		{"bad signature", "Bearer " + valid[:len(valid)-4] + "AAAA", 0, "", "invalid or expired token"},
		{"alg none", "Bearer " + none, 0, "", "invalid or expired token"},
		{"wrong alg", "Bearer " + sign(`{"alg":"HS512"}`, `{"sub":"1","username":"u","exp":1800003600}`), 0, "", "invalid or expired token"},
		{"expired", "Bearer " + sign(hs256, `{"sub":"1","username":"u","exp":1799996400}`), 0, "", "invalid or expired token"},
		{"not yet valid", "Bearer " + sign(hs256, `{"sub":"1","username":"u","nbf":1800000100,"exp":1800003600}`), 0, "", "invalid or expired token"},
		{"payload not json", "Bearer " + sign(hs256, `nope`), 0, "", "invalid or expired token"},
		{"sub not integer", "Bearer " + sign(hs256, `{"sub":"abc","username":"u","exp":1800003600}`), 0, "", "invalid token payload"},
		{"sub is number", "Bearer " + sign(hs256, `{"sub":1,"username":"u","exp":1800003600}`), 0, "", "invalid token payload"},
		{"sub zero", "Bearer " + sign(hs256, `{"sub":"0","username":"u","exp":1800003600}`), 0, "", "invalid token payload"},
		{"no username", "Bearer " + sign(hs256, `{"sub":"1","exp":1800003600}`), 0, "", "invalid token payload"},
		{"username not string", "Bearer " + sign(hs256, `{"sub":"1","username":7,"exp":1800003600}`), 0, "", "invalid token payload"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			id, name, msg := authenticate([]byte(tt.header), now)
			if id != tt.wantID || name != tt.wantName || msg != tt.wantMsg {
				t.Fatalf("got (%d, %q, %q), want (%d, %q, %q)", id, name, msg, tt.wantID, tt.wantName, tt.wantMsg)
			}
		})
	}
}
