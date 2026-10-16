package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"testing"
)

func sign(payload string) string {
	e := base64.RawURLEncoding.EncodeToString
	msg := e([]byte(`{"alg":"HS256","typ":"JWT"}`)) + "." + e([]byte(payload))
	m := hmac.New(sha256.New, jwtSecret)
	m.Write([]byte(msg))
	return msg + "." + e(m.Sum(nil))
}

func TestJWTAndHelpers(t *testing.T) {
	jwtSecret = []byte("s")
	uid, name, ok, pok := verifyJWT(sign(`{"sub":"7","username":"bob","exp":200}`), 100)
	if !ok || !pok || uid != 7 || name != "bob" {
		t.Fatal("valid token rejected")
	}
	if _, _, ok, _ := verifyJWT(sign(`{"sub":"7","username":"bob","exp":50}`), 100); ok {
		t.Fatal("expired accepted")
	}
	if _, _, ok, pok := verifyJWT(sign(`{"sub":"abc","username":"bob","exp":200}`), 100); !ok || pok {
		t.Fatal("bad sub should be payload error")
	}
	if _, _, ok, _ := verifyJWT(sign(`{}`)+"x", 100); ok {
		t.Fatal("bad signature accepted")
	}
	for _, s := range []string{"0", "-1", "abc", "1.5", ""} {
		if _, ok := parseID(s); ok {
			t.Fatal("accepted id " + s)
		}
	}
	if got := string(appendString(nil, []byte("a\"b\\c\n\x01"))); got != `"a\"b\\c\n\u0001"` {
		t.Fatal(got)
	}
}
