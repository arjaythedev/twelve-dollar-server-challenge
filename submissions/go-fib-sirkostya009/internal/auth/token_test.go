package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"strconv"
	"testing"
	"time"
)

var testSecret = []byte("twelve-dollar-challenge")

func sign(payload string) string {
	enc := base64.RawURLEncoding
	h := enc.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	p := enc.EncodeToString([]byte(payload))
	m := hmac.New(sha256.New, testSecret)
	m.Write([]byte(h + "." + p))
	return h + "." + p + "." + enc.EncodeToString(m.Sum(nil))
}

func validToken() string {
	exp := strconv.FormatInt(time.Now().Add(time.Hour).Unix(), 10)
	return sign(`{"sub":"1","username":"golden_ember_1","iat":1,"exp":` + exp + `}`)
}

func TestVerify_Valid_ReturnsPrincipal(t *testing.T) {
	p, err := NewVerifier(testSecret).Verify(validToken())
	if err != nil || p.UserID != 1 || p.Username != "golden_ember_1" {
		t.Fatalf("Verify = %+v, %v", p, err)
	}
}

func BenchmarkVerify(b *testing.B) {
	v := NewVerifier(testSecret)
	tok := validToken()
	b.ReportAllocs()
	for b.Loop() {
		if _, err := v.Verify(tok); err != nil {
			b.Fatal(err)
		}
	}
}
