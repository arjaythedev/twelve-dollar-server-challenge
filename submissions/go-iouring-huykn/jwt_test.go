package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
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

func TestRejectInvalidTokenFraming(t *testing.T) {
	v := newVerifier([]byte("test"))
	valid := signedAuth(`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1800000000}`)
	alphabet := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
	canonical := append([]byte(nil), valid...)
	for i := range alphabet {
		if alphabet[i] == canonical[len(canonical)-1] {
			canonical[len(canonical)-1] = alphabet[i+1] // Same decoded bits, nonzero padding bits.
			break
		}
	}
	for _, value := range [][]byte{
		append(append([]byte(nil), valid...), '='),
		append(append([]byte(nil), valid...), '\n'),
		append(append([]byte(nil), valid...), '.'),
		[]byte("Bearer .."), canonical,
		signedAuth(`{"alg":"none"}`, `{"sub":"1","username":"alice","exp":1800000000}`),
		signedAuth(`{"alg":"HS256"}`, `{"sub":"1","username":"alice"}`),
		signedAuth(`{"alg":"HS256"}`, `{"sub":"1","username":"alice","exp":1800000000}junk`),
	} {
		if status, _, _ := v.verify(value, true, 1700000000); status != authInvalid {
			t.Fatalf("accepted %q: %d", value, status)
		}
	}
	for _, input := range []string{"0", "01", "+1", "-1", "1.0", "9007199254740993"} {
		if _, ok := positive([]byte(input)); ok {
			t.Fatalf("accepted id %q", input)
		}
	}
}

func TestVerifierLongKeyAndWorkspace(t *testing.T) {
	key := make([]byte, 200)
	for i := range key {
		key[i] = byte(i)
	}
	v := newVerifier(key)
	for _, name := range []string{"alice", string(make([]byte, 5000)), "again"} {
		// Marshal the embedded NULs as JSON escapes.
		encodedName, err := json.Marshal(name)
		if err != nil {
			t.Fatal(err)
		}
		header := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256"}`))
		payload := base64.RawURLEncoding.EncodeToString([]byte(`{"sub":"1","username":` + string(encodedName) + `,"exp":1800000000}`))
		token := header + "." + payload
		mac := hmac.New(sha256.New, key)
		mac.Write([]byte(token))
		authorization := []byte("Bearer " + token + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil)))
		status, id, got := v.verify(authorization, true, 1700000000)
		if status != authOK || id != 1 || string(got) != name {
			t.Fatalf("key/workspace: %d %d %q", status, id, got)
		}
	}
}
