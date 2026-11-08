package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"testing"
)

func signedAuth(header, payload string) []byte {
	enc := base64.RawURLEncoding
	token := enc.EncodeToString([]byte(header)) + "." + enc.EncodeToString([]byte(payload))
	mac := hmac.New(sha256.New, []byte("test"))
	mac.Write([]byte(token))
	return []byte("Bearer " + token + "." + enc.EncodeToString(mac.Sum(nil)))
}

func TestVerifyClaims(t *testing.T) {
	v := newVerifier([]byte("test"))
	for _, tc := range []struct {
		header, payload string
		want            int
	}{
		{`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1800000000}`, authOK},
		{`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1800000000.0e-10}`, authInvalid},
		{`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1.8e9}`, authOK},
		{`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1700000000}`, authInvalid},
		{`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":99999999999999999e-20}`, authInvalid},
		{`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":0.000e9999}`, authInvalid},
		{`{"alg":"HS\u003256"}`, `{"sub":"\u0031","username":"al\u0069ce","exp":1800000000}`, authOK},
		{`{"alg":"HS384"}`, `{"sub":"1","username":"alice","exp":1800000000}`, authInvalid},
		{`{"alg":"HS256"}`, `{"sub":1,"username":"alice","exp":1800000000}`, authPayload},
	} {
		status, uid, name := v.verify(signedAuth(tc.header, tc.payload), true, 1700000000)
		if status != tc.want {
			t.Errorf("%s %s: status %d, want %d", tc.header, tc.payload, status, tc.want)
		}
		if status == authOK && (uid != 1 || string(name) != "alice") {
			t.Errorf("decoded claims: uid=%d name=%q", uid, name)
		}
	}
	auth := signedAuth(`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1800000000}`)
	auth[len(auth)-2] ^= 1
	if status, _, _ := v.verify(auth, true, 1700000000); status != authInvalid {
		t.Fatal("accepted tampered signature")
	}
}

func BenchmarkVerify(b *testing.B) {
	v := newVerifier([]byte("test"))
	auth := signedAuth(`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1800000000}`)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if status, _, _ := v.verify(auth, true, 1700000000); status != authOK {
			b.Fatal(status)
		}
	}
}
