//go:build linux

package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"strings"
	"testing"
	"time"
)

// The allocation-free token check must give exactly the answer of the standard-library
// version (authSlow) for every token, hostile ones included. Each case below is checked
// both ways and the results compared.
func TestAuthFastMatchesSlow(t *testing.T) {
	secret := []byte("twelve-dollar-challenge")
	s := &server{secret: secret, verify: newVerifier(secret)}
	enc := base64.RawURLEncoding.EncodeToString
	sign := func(h, p string) string {
		m := hmac.New(sha256.New, secret)
		m.Write([]byte(h + "." + p))
		return enc(m.Sum(nil))
	}
	tok := func(hdr, claims string) string { // correctly signed token from two JSON texts
		h, p := enc([]byte(hdr)), enc([]byte(claims))
		return h + "." + p + "." + sign(h, p)
	}
	future := time.Now().Unix() + 3600
	past := time.Now().Unix() - 10
	hs := `{"alg":"HS256","typ":"JWT"}`
	good := fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d}`, future)
	gh, gp := enc([]byte(hs)), enc([]byte(good))
	gs := sign(gh, gp)

	cases := map[string]string{
		"good":                    tok(hs, good),
		"good, other key order":   tok(`{"typ":"JWT","alg":"HS256"}`, fmt.Sprintf(`{"exp":%d,"username":"ada","sub":"42"}`, future)),
		"good, with iat":          tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","iat":%d,"exp":%d}`, past, future)),
		"good, spaces in JSON":    tok(`{ "alg" : "HS256" }`, fmt.Sprintf("{ \"sub\" : \"42\" ,\n\t\"username\":\"ada\", \"exp\": %d }", future)),
		"no exp":                  tok(hs, `{"sub":"42","username":"ada"}`),
		"exp in the past":         tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d}`, past)),
		"exp now":                 tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d}`, time.Now().Unix())),
		"exp as string":           tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":"%d"}`, future)),
		"exp as float":            tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d.5}`, future)),
		"exp with exponent":       tok(hs, `{"sub":"42","username":"ada","exp":1e12}`),
		"exp negative":            tok(hs, `{"sub":"42","username":"ada","exp":-5}`),
		"exp zero":                tok(hs, `{"sub":"42","username":"ada","exp":0}`),
		"exp leading zero":        tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":0%d}`, future)),
		"exp huge":                tok(hs, `{"sub":"42","username":"ada","exp":99999999999999999999999}`),
		"exp null":                tok(hs, `{"sub":"42","username":"ada","exp":null}`),
		"exp twice":               tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d,"exp":%d}`, past, future)),
		"exp twice, other order":  tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d,"exp":%d}`, future, past)),
		"alg none":                tok(`{"alg":"none"}`, good),
		"alg HS512":               tok(`{"alg":"HS512"}`, good),
		"alg missing":             tok(`{"typ":"JWT"}`, good),
		"alg lower case value":    tok(`{"alg":"hs256"}`, good),
		"alg as number":           tok(`{"alg":256}`, good),
		"alg twice":               tok(`{"alg":"none","alg":"HS256"}`, good),
		"alg twice, other order":  tok(`{"alg":"HS256","alg":"none"}`, good),
		"ALG upper-case key":      tok(`{"ALG":"HS256"}`, good),
		"alg and ALG":             tok(`{"alg":"none","ALG":"HS256"}`, good),
		"ALG and alg":             tok(`{"ALG":"none","alg":"HS256"}`, good),
		"header not an object":    tok(`"HS256"`, good),
		"header empty object":     tok(`{}`, good),
		"header not JSON":         tok(`{alg:HS256}`, good),
		"header trailing garbage": tok(`{"alg":"HS256"}x`, good),
		"header nested value":     tok(`{"alg":"HS256","jwk":{"k":1}}`, good),
		"header escaped key":      tok(`{"\u0061lg":"HS256"}`, good),
		"header escaped value":    tok(`{"alg":"HS\u003256"}`, good),
		"claims not an object":    tok(hs, `[1,2,3]`),
		"claims not JSON":         tok(hs, `{"sub":"42",}`),
		"claims with bool":        tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","admin":true,"exp":%d}`, future)),
		"claims with nested":      tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","x":{"exp":1},"exp":%d}`, future)),
		"sub missing":             tok(hs, fmt.Sprintf(`{"username":"ada","exp":%d}`, future)),
		"sub as number":           tok(hs, fmt.Sprintf(`{"sub":42,"username":"ada","exp":%d}`, future)),
		"sub not numeric":         tok(hs, fmt.Sprintf(`{"sub":"abc","username":"ada","exp":%d}`, future)),
		"sub zero":                tok(hs, fmt.Sprintf(`{"sub":"0","username":"ada","exp":%d}`, future)),
		"sub negative":            tok(hs, fmt.Sprintf(`{"sub":"-1","username":"ada","exp":%d}`, future)),
		"sub leading zero":        tok(hs, fmt.Sprintf(`{"sub":"007","username":"ada","exp":%d}`, future)),
		"sub too large":           tok(hs, fmt.Sprintf(`{"sub":"99999999999999999999","username":"ada","exp":%d}`, future)),
		"sub max int64":           tok(hs, fmt.Sprintf(`{"sub":"9223372036854775807","username":"ada","exp":%d}`, future)),
		"sub with space":          tok(hs, fmt.Sprintf(`{"sub":" 42","username":"ada","exp":%d}`, future)),
		"sub empty":               tok(hs, fmt.Sprintf(`{"sub":"","username":"ada","exp":%d}`, future)),
		"sub twice":               tok(hs, fmt.Sprintf(`{"sub":"1","sub":"2","username":"ada","exp":%d}`, future)),
		"username missing":        tok(hs, fmt.Sprintf(`{"sub":"42","exp":%d}`, future)),
		"username as number":      tok(hs, fmt.Sprintf(`{"sub":"42","username":7,"exp":%d}`, future)),
		"username empty":          tok(hs, fmt.Sprintf(`{"sub":"42","username":"","exp":%d}`, future)),
		"username escaped":        tok(hs, fmt.Sprintf(`{"sub":"42","username":"a\"d\\a\u00e9","exp":%d}`, future)),
		"username UTF-8":          tok(hs, fmt.Sprintf(`{"sub":"42","username":"адa✓","exp":%d}`, future)),
		"username bad UTF-8":      tok(hs, fmt.Sprintf("{\"sub\":\"42\",\"username\":\"a\xffb\",\"exp\":%d}", future)),
		"username twice":          tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","username":"bob","exp":%d}`, future)),
		"SUB upper-case key":      tok(hs, fmt.Sprintf(`{"SUB":"42","username":"ada","exp":%d}`, future)),
		"Sub before sub":          tok(hs, fmt.Sprintf(`{"Sub":"7","sub":"42","username":"ada","exp":%d}`, future)),
		"username and Username":   tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","Username":"bob","exp":%d}`, future)),
		"EXP after exp":           tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d,"EXP":%d}`, future, past)),
		"key with long s":         tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","uſername":"bob","exp":%d}`, future)),
		"other key UTF-8":         tok(hs, fmt.Sprintf(`{"sub":"42","username":"ada","né":"x","exp":%d}`, future)),
		"other key bad UTF-8":     tok(hs, fmt.Sprintf("{\"sub\":\"42\",\"username\":\"ada\",\"n\xff\":\"x\",\"exp\":%d}", future)),
		"other value bad UTF-8":   tok(hs, fmt.Sprintf("{\"sub\":\"42\",\"username\":\"ada\",\"n\":\"x\xff\",\"exp\":%d}", future)),
		"username control char":   tok(hs, fmt.Sprintf("{\"sub\":\"42\",\"username\":\"a\x01b\",\"exp\":%d}", future)),
		"wrong signature":         gh + "." + gp + "." + enc(make([]byte, 32)),
		"signature of other head": gh + "." + gp + "." + sign(enc([]byte(`{"alg":"HS256"}`)), gp),
		"signature truncated":     gh + "." + gp + "." + gs[:20],
		"signature empty":         gh + "." + gp + ".",
		"signature too long":      gh + "." + gp + "." + enc(make([]byte, 100)),
		"signature padded":        gh + "." + gp + "." + gs + "=",
		"signature std alphabet":  gh + "." + gp + "." + strings.NewReplacer("-", "+", "_", "/").Replace(gs),
		"signature trailing bits": gh + "." + gp + "." + gs[:len(gs)-1] + "B",
		"header padded":           gh + "=." + gp + "." + sign(gh+"=", gp),
		"claims padded":           gh + "." + gp + "==." + sign(gh, gp+"=="),
		"header bad base64":       "!!!." + gp + "." + sign("!!!", gp),
		"claims bad base64":       gh + ".***." + sign(gh, "***"),
		"no dots":                 gh,
		"one dot":                 gh + "." + gp,
		"three dots":              gh + "." + gp + "." + gs + ".x",
		"four parts, valid first": gh + "." + gp + "." + gs + "." + gs,
		"empty header":            "." + gp + "." + sign("", gp),
		"empty claims":            gh + ".." + sign(gh, ""),
		"only dots":               "..",
		"10 KB claims":            tok(hs, fmt.Sprintf(`{"sub":"42","username":"%s","exp":%d}`, strings.Repeat("a", 10000), future)),
		"10 KB header":            tok(fmt.Sprintf(`{"alg":"HS256","kid":"%s"}`, strings.Repeat("k", 10000)), good),
		"claims exactly at limit": tok(hs, fmt.Sprintf(`{"sub":"42","username":"%s","exp":%d}`, strings.Repeat("a", 1024-45), future)),
	}
	accepted := 0
	for name, token := range cases {
		head := []byte("POST /posts/1/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token)
		uid, user, msg := s.auth(head)
		got := fmt.Sprintf("%d %q %q", uid, user, msg)
		uid, user, msg = s.authSlow([]byte(token))
		want := fmt.Sprintf("%d %q %q", uid, user, msg)
		if got != want {
			t.Errorf("%s:\n  fast: %s\n  slow: %s", name, got, want)
		}
		if msg == "" {
			accepted++
		}
	}
	if accepted < 5 || accepted > len(cases)/2 {
		t.Errorf("%d of %d cases accepted: the table is not testing what it should", accepted, len(cases))
	}
	for _, h := range []string{"GET / HTTP/1.1\r\nHost: x", "GET / HTTP/1.1\r\nAuthorization: Basic abc", "GET / HTTP/1.1\r\nAuthorization: bearer abc",
		"GET / HTTP/1.1\r\nAuthorization: Bearer ", "GET / HTTP/1.1\r\nAuthorization: Bearer"} { // an empty token is no token
		if _, _, msg := s.auth([]byte(h)); msg != "missing bearer token" {
			t.Errorf("%q: %q", h, msg)
		}
	}
}

// The fast path must not allocate for an ordinary token.
func TestAuthDoesNotAllocate(t *testing.T) {
	secret := []byte("twelve-dollar-challenge")
	s := &server{secret: secret, verify: newVerifier(secret)}
	enc := base64.RawURLEncoding.EncodeToString
	h := enc([]byte(`{"alg":"HS256","typ":"JWT"}`))
	p := enc([]byte(fmt.Sprintf(`{"sub":"42","username":"ada","exp":%d}`, time.Now().Unix()+3600)))
	m := hmac.New(sha256.New, secret)
	m.Write([]byte(h + "." + p))
	head := []byte("POST /posts/1/like HTTP/1.1\r\nAuthorization: Bearer " + h + "." + p + "." + enc(m.Sum(nil)))
	if uid, _, msg := s.auth(head); uid != 42 || msg != "" {
		t.Fatalf("uid=%d msg=%q", uid, msg)
	}
	if n := testing.AllocsPerRun(1000, func() { s.auth(head) }); n != 0 {
		t.Errorf("%v allocations per token check, want 0", n)
	}
}
