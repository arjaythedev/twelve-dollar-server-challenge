package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"testing"
	"time"
)

func signJWT(payload string, secret []byte) string {
	enc := base64.RawURLEncoding
	h := enc.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	p := enc.EncodeToString([]byte(payload))
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(h + "." + p))
	s := enc.EncodeToString(mac.Sum(nil))
	return h + "." + p + "." + s
}

func TestJWT(t *testing.T) {
	secret := []byte("twelve-dollar-challenge")
	setJWTSecret(secret)
	now := time.Now().Unix()

	// 1. Valid token
	validTok := []byte(signJWT(fmt.Sprintf(`{"sub":"123","username":"alice","iat":%d,"exp":%d}`, now, now+3600), secret))
	uid, user, ok, plOK := verifyJWT(validTok, now)
	if !ok || !plOK || uid != 123 || string(user) != "alice" {
		t.Fatalf("expected valid token, got uid=%d user=%s ok=%v plOK=%v", uid, user, ok, plOK)
	}

	// 2. Expired token (standard)
	expTok := []byte(signJWT(fmt.Sprintf(`{"sub":"123","username":"alice","iat":%d,"exp":%d}`, now-7200, now-3600), secret))
	_, _, ok, _ = verifyJWT(expTok, now)
	if ok {
		t.Fatalf("expected expired token to fail")
	}

	// 3. Expired token with username before exp (regression test for key order bug)
	expBeforeTok := []byte(signJWT(fmt.Sprintf(`{"username":"expert","exp":%d,"sub":"123"}`, now-3600), secret))
	_, _, ok, _ = verifyJWT(expBeforeTok, now)
	if ok {
		t.Fatalf("expected expired token with username before exp to fail")
	}

	// 4. Token with 'exp' inside username string (regression test for substring match bug)
	expInStrTok := []byte(signJWT(fmt.Sprintf(`{"username":"my_exp:100","sub":"123","exp":%d}`, now-3600), secret))
	_, _, ok, _ = verifyJWT(expInStrTok, now)
	if ok {
		t.Fatalf("expected expired token with 'exp' inside username to fail")
	}

	// 5. Sub not integer string (e.g. "abc")
	badSubTok := []byte(signJWT(fmt.Sprintf(`{"sub":"abc","username":"alice","iat":%d,"exp":%d}`, now, now+3600), secret))
	_, _, ok, plOK = verifyJWT(badSubTok, now)
	if !ok || plOK {
		t.Fatalf("expected bad sub to have ok=true, plOK=false, got ok=%v plOK=%v", ok, plOK)
	}

	// 6. Sub is a number, not a string: {"sub":123}
	numSubTok := []byte(signJWT(fmt.Sprintf(`{"sub":123,"username":"alice","iat":%d,"exp":%d}`, now, now+3600), secret))
	_, _, ok, plOK = verifyJWT(numSubTok, now)
	if !ok || plOK {
		t.Fatalf("expected numeric sub to have ok=true, plOK=false, got ok=%v plOK=%v", ok, plOK)
	}

	// 7. No username
	noNameTok := []byte(signJWT(fmt.Sprintf(`{"sub":"123","iat":%d,"exp":%d}`, now, now+3600), secret))
	_, _, ok, plOK = verifyJWT(noNameTok, now)
	if !ok || plOK {
		t.Fatalf("expected no name to have ok=true, plOK=false, got ok=%v plOK=%v", ok, plOK)
	}

	// 8. Bad signature
	badSigTok := append(validTok[:len(validTok)-4], "AAAA"...)
	_, _, ok, _ = verifyJWT(badSigTok, now)
	if ok {
		t.Fatalf("expected bad signature to fail")
	}

	// 9. Alg none
	enc := base64.RawURLEncoding
	noneH := enc.EncodeToString([]byte(`{"alg":"none","typ":"JWT"}`))
	noneP := enc.EncodeToString([]byte(fmt.Sprintf(`{"sub":"123","username":"alice","exp":%d}`, now+3600)))
	noneTok := []byte(noneH + "." + noneP + ".")
	_, _, ok, _ = verifyJWT(noneTok, now)
	if ok {
		t.Fatalf("expected alg:none to fail")
	}

	// 10. Quote escaping in username: ev"il
	evilTok := []byte(signJWT(fmt.Sprintf(`{"sub":"999","username":"ev\"il","exp":%d}`, now+3600), secret))
	uid, evilUser, ok, plOK := verifyJWT(evilTok, now)
	if !ok || !plOK || uid != 999 {
		t.Fatalf("expected evil username token to pass verification")
	}
	// Verify that appendPost creates 100% valid JSON even with evil quotes
	res := appendPost(nil, 1, []byte("test body"), []byte("2026-10-13T00:00:00.000Z"), evilUser, 0)
	var parsed map[string]any
	if err := json.Unmarshal(res, &parsed); err != nil {
		t.Fatalf("appendPost produced invalid JSON for author with quotes: %v (raw: %s)", err, string(res))
	}
	if parsed["author"] != `ev"il` {
		t.Fatalf("expected parsed author to be ev\"il, got: %v", parsed["author"])
	}

	// 11. Unicode quote escaping \u0022 and escaped quote in keys
	unicodeTok := []byte(signJWT(fmt.Sprintf(`{"sub":"888","extra\"key":"test","username":"ev\u0022il","exp":%d}`, now+3600), secret))
	uid, unicodeUser, ok, plOK := verifyJWT(unicodeTok, now)
	if !ok || !plOK || uid != 888 || string(unicodeUser) != `ev"il` {
		t.Fatalf("expected unicode escaped quote token to pass, got uid=%d user=%s ok=%v plOK=%v", uid, unicodeUser, ok, plOK)
	}
}

func BenchmarkVerifyJWT(b *testing.B) {
	secret := []byte("twelve-dollar-challenge")
	setJWTSecret(secret)
	now := time.Now().Unix()
	validTok := []byte(signJWT(fmt.Sprintf(`{"sub":"123","username":"alice","iat":%d,"exp":%d}`, now, now+3600), secret))

	b.ResetTimer()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		_, _, ok, plOK := verifyJWT(validTok, now)
		if !ok || !plOK {
			b.Fatal("failed")
		}
	}
}
