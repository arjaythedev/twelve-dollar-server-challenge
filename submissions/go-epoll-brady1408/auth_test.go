package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"testing"
	"time"
)

var testSecret = []byte("twelve-dollar-challenge")

func b64(s string) string { return base64.RawURLEncoding.EncodeToString([]byte(s)) }

func signed(header, payload string) string {
	in := b64(header) + "." + b64(payload)
	m := hmac.New(sha256.New, testSecret)
	m.Write([]byte(in))
	return in + "." + base64.RawURLEncoding.EncodeToString(m.Sum(nil))
}

func TestAuthenticate(t *testing.T) {
	now := time.Now().Unix()
	hs := `{"alg":"HS256","typ":"JWT"}`
	good := signed(hs, fmt.Sprintf(`{"sub":"1","username":"golden_ember_1","iat":%d,"exp":%d}`, now, now+3600))
	cases := []struct {
		name   string
		header string
		want   authStatus
	}{
		{"no header", "", authMissing},
		{"basic", "Basic abc", authMissing},
		{"bearer only", "Bearer", authMissing},
		{"good", "Bearer " + good, authOK},
		{"garbage", "Bearer abc", authInvalid},
		{"two parts", "Bearer a.b", authInvalid},
		{"bad signature same length", "Bearer " + good[:len(good)-4] + "AAAA", authInvalid},
		{"wrong-length signature", "Bearer " + good[:len(good)-43] + "c2lnbmF0dXJlLWlzLW5vdC12YWxpZC1hdC1hbGwtYXQtYWxsLTEyMzQ", authInvalid},
		{"alg none", "Bearer " + b64(`{"alg":"none","typ":"JWT"}`) + "." + b64(`{"sub":"1","username":"x","exp":9999999999}`) + ".", authInvalid},
		{"alg RS256 but HS-signed", "Bearer " + signed(`{"alg":"RS256"}`, `{"sub":"1","username":"x","exp":9999999999}`), authInvalid},
		{"expired", "Bearer " + signed(hs, fmt.Sprintf(`{"sub":"1","username":"x","iat":%d,"exp":%d}`, now-7200, now-3600)), authInvalid},
		{"no exp", "Bearer " + signed(hs, `{"sub":"1","username":"x"}`), authInvalid},
		{"payload not json", "Bearer " + signed(hs, `{sub`), authInvalid},
		{"sub not integer", "Bearer " + signed(hs, fmt.Sprintf(`{"sub":"abc","username":"x","exp":%d}`, now+60)), authBadPayload},
		{"sub zero", "Bearer " + signed(hs, fmt.Sprintf(`{"sub":"0","username":"x","exp":%d}`, now+60)), authBadPayload},
		{"sub numeric ok", "Bearer " + signed(hs, fmt.Sprintf(`{"sub":7,"username":"x","exp":%d}`, now+60)), authOK},
		{"no username", "Bearer " + signed(hs, fmt.Sprintf(`{"sub":"1","exp":%d}`, now+60)), authBadPayload},
		{"username not string", "Bearer " + signed(hs, fmt.Sprintf(`{"sub":"1","username":5,"exp":%d}`, now+60)), authBadPayload},
	}
	v := newVerifier(testSecret)
	for _, c := range cases {
		var hdr []byte
		if c.header != "" {
			hdr = []byte(c.header)
		}
		got, st := v.authenticate(hdr, now)
		if st != c.want {
			t.Errorf("%s: status %v, want %v", c.name, st, c.want)
		}
		if c.name == "good" && (got.userID != 1 || got.username != "golden_ember_1") {
			t.Errorf("good claims: %+v", got)
		}
	}
}
