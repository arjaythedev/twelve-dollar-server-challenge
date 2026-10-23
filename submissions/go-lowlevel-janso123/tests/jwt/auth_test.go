// SPDX-License-Identifier: MIT
// JWT regression cases adapted from the reviewed local source harness.
package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"
)

var secret = []byte("test secret")

const header = `{"alg":"HS256"}`
const claims = `{"sub":"1","username":"fixture","iat":0,"exp":4102444800}`
const invalid = "invalid or expired token"
const payloadError = "invalid token payload"

type result struct {
	id        int64
	name, err string
}

type authFunc func([]byte, string) (int64, string, string)

func call(fn authFunc, key []byte, auth string) result {
	id, name, err := fn(key, auth)
	return result{id, name, err}
}

func signedParts(head, body string) string {
	s := head + "." + body
	m := hmac.New(sha256.New, secret)
	m.Write([]byte(s))
	return "Bearer " + s + "." + base64.RawURLEncoding.EncodeToString(m.Sum(nil))
}

func token(head, body string) string {
	return signedParts(base64.RawURLEncoding.EncodeToString([]byte(head)), base64.RawURLEncoding.EncodeToString([]byte(body)))
}

type testCase struct {
	name, auth string
	want       result
}

// These literal expectations catch weakened signature/claim/JSON checks and
// changed public error precedence. The frozen implementation is also checked.
func cases() []testCase {
	good := token(header, claims)
	encodedHead := base64.RawURLEncoding.EncodeToString([]byte(header))
	encodedClaims := base64.RawURLEncoding.EncodeToString([]byte(claims))
	cs := []testCase{
		{"valid", good, result{1, "fixture", ""}},
		{"no header", "", result{err: "missing bearer token"}},
		{"wrong prefix", strings.Replace(good, "Bearer", "bearer", 1), result{err: "missing bearer token"}},
		{"no prefix space", "Bearer", result{err: "missing bearer token"}},
		{"empty token", "Bearer ", result{err: invalid}},
		{"extra prefix space", "Bearer  " + good[7:], result{err: invalid}},
		{"zero dots", "Bearer abc", result{err: invalid}},
		{"one dot", "Bearer abc.def", result{err: invalid}},
		{"three dots", good + ".x", result{err: invalid}},
		{"many dots", "Bearer " + strings.Repeat(".", 1000), result{err: invalid}},
		{"empty all", "Bearer ..", result{err: invalid}},
		{"empty head", signedParts("", encodedClaims), result{err: invalid}},
		{"empty claims", signedParts(encodedHead, ""), result{err: invalid}},
		{"signature empty", good[:strings.LastIndexByte(good, '.')+1], result{err: invalid}},
		{"header tampered", strings.Replace(good, encodedHead, base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","extra":1}`)), 1), result{err: invalid}},
		{"claims tampered", strings.Replace(good, encodedClaims, base64.RawURLEncoding.EncodeToString([]byte(strings.Replace(claims, `"1"`, `"2"`, 1))), 1), result{err: invalid}},
	}
	addBody := func(name, body, err string) { cs = append(cs, testCase{name, token(header, body), result{err: err}}) }
	for _, body := range []struct{ name, body, err string }{
		{"missing exp", `{"sub":"1","username":"fixture"}`, invalid},
		{"string exp", `{"sub":"1","username":"fixture","exp":"4102444800"}`, invalid},
		{"null exp", `{"sub":"1","username":"fixture","exp":null}`, invalid},
		{"bool exp", `{"sub":"1","username":"fixture","exp":true}`, invalid},
		{"array exp", `{"sub":"1","username":"fixture","exp":[]}`, invalid},
		{"object exp", `{"sub":"1","username":"fixture","exp":{}}`, invalid},
		{"expired", `{"sub":"1","username":"fixture","exp":1}`, invalid},
		{"fraction expired", `{"sub":"1","username":"fixture","exp":1.5}`, invalid},
		{"zero exp", `{"sub":"1","username":"fixture","exp":0}`, invalid},
		{"negative exp", `{"sub":"1","username":"fixture","exp":-1}`, invalid},
		{"future nbf", `{"sub":"1","username":"fixture","exp":4102444800,"nbf":4102444700}`, invalid},
		{"string nbf", `{"sub":"1","username":"fixture","exp":4102444800,"nbf":"0"}`, invalid},
		{"null nbf", `{"sub":"1","username":"fixture","exp":4102444800,"nbf":null}`, invalid},
		{"bool nbf", `{"sub":"1","username":"fixture","exp":4102444800,"nbf":false}`, invalid},
		{"array nbf", `{"sub":"1","username":"fixture","exp":4102444800,"nbf":[]}`, invalid},
		{"object nbf", `{"sub":"1","username":"fixture","exp":4102444800,"nbf":{}}`, invalid},
		{"missing sub", `{"username":"fixture","exp":4102444800}`, payloadError},
		{"number sub", `{"sub":1,"username":"fixture","exp":4102444800}`, payloadError},
		{"null sub", `{"sub":null,"username":"fixture","exp":4102444800}`, payloadError},
		{"empty sub", `{"sub":"","username":"fixture","exp":4102444800}`, payloadError},
		{"zero sub", `{"sub":"0","username":"fixture","exp":4102444800}`, payloadError},
		{"negative sub", `{"sub":"-1","username":"fixture","exp":4102444800}`, payloadError},
		{"exponent sub", `{"sub":"1e0","username":"fixture","exp":4102444800}`, payloadError},
		{"overflow sub", `{"sub":"9223372036854775808","username":"fixture","exp":4102444800}`, payloadError},
		{"unsafe integer sub", `{"sub":"9007199254740992","username":"fixture","exp":4102444800}`, payloadError},
		{"missing username", `{"sub":"1","exp":4102444800}`, payloadError},
		{"number username", `{"sub":"1","username":1,"exp":4102444800}`, payloadError},
		{"null username", `{"sub":"1","username":null,"exp":4102444800}`, payloadError},
		{"object username", `{"sub":"1","username":{},"exp":4102444800}`, payloadError},
		{"array username", `{"sub":"1","username":[],"exp":4102444800}`, payloadError},
		{"claims null", `null`, invalid},
		{"claims array", `[]`, invalid},
		{"claims string", `"text"`, invalid},
		{"claims number", `1`, invalid},
		{"claims bool", `true`, invalid},
		{"trailing JSON", claims + `{}`, invalid},
		{"trailing garbage", claims + `x`, invalid},
		{"truncated JSON", claims[:len(claims)-1], invalid},
		{"unknown malformed member", strings.TrimSuffix(claims, "}") + `,"x":[}`, invalid},
		{"overflow unknown number", strings.TrimSuffix(claims, "}") + `,"x":1e9999}`, invalid},
		{"invalid UTF8 name", strings.Replace(claims, "fixture", "\xff", 1), invalid},
		{"invalid UTF8 unknown key", strings.TrimSuffix(claims, "}") + ",\"\xff\":0}", invalid},
		{"exp validation before sub", `{"sub":0,"exp":1}`, invalid},
	} {
		addBody(body.name, body.body, body.err)
	}
	for _, h := range []string{`{}`, `null`, `[]`, `1`, `true`, `"HS256"`, `{"alg":"none"}`, `{"alg":"HS512"}`, `{"alg":null}`, `{"alg":1}`, `{"alg":{}}`, `{"alg":[]}`, header + `{}`, header + `x`, `{"alg":"HS256","unknown":1e9999}`, "{\"alg\":\"HS256\",\"x\":\"\xff\"}"} {
		cs = append(cs, testCase{"bad header " + h, token(h, claims), result{err: invalid}})
	}
	for _, accepted := range []struct {
		name, head, body string
		want             result
	}{
		{"empty username", header, strings.Replace(claims, "fixture", "", 1), result{1, "", ""}},
		{"UTF8 username", header, strings.Replace(claims, "fixture", "Żółć ☃", 1), result{1, "Żółć ☃", ""}},
		{"escaped username", header, strings.Replace(claims, "fixture", `a\n\u2603`, 1), result{1, "a\n☃", ""}},
		{"leading zero sub", header, strings.Replace(claims, `"1"`, `"001"`, 1), result{1, "fixture", ""}},
		{"max sub", header, strings.Replace(claims, `"1"`, `"9007199254740991"`, 1), result{9007199254740991, "fixture", ""}},
		{"past nbf", header, strings.TrimSuffix(claims, "}") + `,"nbf":1.25}`, result{1, "fixture", ""}},
		{"fraction exp", header, strings.Replace(claims, "4102444800", "4102444800.5", 1), result{1, "fixture", ""}},
		{"exponent exp", header, strings.Replace(claims, "4102444800", "4.1024448e9", 1), result{1, "fixture", ""}},
		{"JSON whitespace", " \n" + header + "\t", "\r" + claims + "\n ", result{1, "fixture", ""}},
		{"duplicate alg last valid", `{"alg":"none","alg":"HS256"}`, claims, result{1, "fixture", ""}},
		{"duplicate sub last valid", header, strings.Replace(claims, `"sub":"1"`, `"sub":false,"sub":"1"`, 1), result{1, "fixture", ""}},
		{"duplicate username last wins", header, strings.Replace(claims, `"username":"fixture"`, `"username":"first","username":"fixture"`, 1), result{1, "fixture", ""}},
		{"duplicate exp last valid", header, strings.Replace(claims, `"exp":4102444800`, `"exp":0,"exp":4102444800`, 1), result{1, "fixture", ""}},
		{"duplicate nbf last valid", header, strings.TrimSuffix(claims, "}") + `,"nbf":4102444800,"nbf":0}`, result{1, "fixture", ""}},
		{"unknown nested data", header, strings.TrimSuffix(claims, "}") + `,"x":{"a":[null,true,{},[]]}}`, result{1, "fixture", ""}},
	} {
		cs = append(cs, testCase{accepted.name, token(accepted.head, accepted.body), accepted.want})
	}
	cs = append(cs, testCase{"duplicate alg last invalid", token(`{"alg":"HS256","alg":"none"}`, claims), result{err: invalid}})
	addBody("duplicate sub last invalid", strings.Replace(claims, `"sub":"1"`, `"sub":"1","sub":1`, 1), payloadError)
	addBody("duplicate username last invalid", strings.Replace(claims, `"username":"fixture"`, `"username":"fixture","username":1`, 1), payloadError)
	addBody("duplicate exp last invalid", strings.Replace(claims, `"exp":4102444800`, `"exp":4102444800,"exp":0`, 1), invalid)
	addBody("duplicate nbf last invalid", strings.TrimSuffix(claims, "}")+`,"nbf":0,"nbf":4102444800}`, invalid)
	// DecodeString alone accepts CR/LF. Re-sign malformed encoded segments so
	// rejection must come from alphabet validation, not a stale signature.
	for _, bad := range []string{"\n", "\r", "\r\n", " ", "\t", "+", "/", "=", "\x00", "\xff"} {
		cs = append(cs,
			testCase{fmt.Sprintf("head alphabet %q", bad), signedParts(encodedHead[:3]+bad+encodedHead[3:], encodedClaims), result{err: invalid}},
			testCase{fmt.Sprintf("claims alphabet %q", bad), signedParts(encodedHead, encodedClaims[:3]+bad+encodedClaims[3:]), result{err: invalid}},
			testCase{fmt.Sprintf("signature alphabet %q", bad), good[:len(good)-10] + bad + good[len(good)-10:], result{err: invalid}},
		)
	}
	for _, part := range []string{"A", "AAAAA", "=", "a=", "____="} {
		cs = append(cs, testCase{"head malformed base64 " + part, signedParts(part, encodedClaims), result{err: invalid}}, testCase{"claims malformed base64 " + part, signedParts(encodedHead, part), result{err: invalid}})
	}
	for _, size := range []int{0, 1, 2, 30, 31, 32, 33, 34, 64, 1024} {
		cs = append(cs, testCase{fmt.Sprintf("wrong signature bytes %d", size), good[:strings.LastIndexByte(good, '.')+1] + base64.RawURLEncoding.EncodeToString(make([]byte, size)), result{err: invalid}})
	}
	signatureAt := strings.LastIndexByte(good, '.') + 1
	for _, pos := range []int{0, 16, 31} {
		signature, _ := base64.RawURLEncoding.DecodeString(good[signatureAt:])
		signature[pos] ^= 1
		cs = append(cs, testCase{fmt.Sprintf("signature bit flipped byte %d", pos), good[:signatureAt] + base64.RawURLEncoding.EncodeToString(signature), result{err: invalid}})
	}
	// RawURLEncoding is deliberately not Strict: low unused bits have always
	// been accepted. Preserve that behavior while requiring a 32-byte signature.
	const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
	last := strings.IndexByte(alphabet, good[len(good)-1])
	for low := 0; low < 4; low++ {
		cs = append(cs, testCase{fmt.Sprintf("noncanonical signature bits %d", low), good[:len(good)-1] + string(alphabet[last&^3|low]), result{1, "fixture", ""}})
	}
	return cs
}

func TestAuthenticationContract(t *testing.T) {
	for _, tc := range cases() {
		t.Run(tc.name, func(t *testing.T) {
			for name, fn := range map[string]authFunc{"source": authenticateSource} {
				if got := call(fn, secret, tc.auth); got != tc.want {
					t.Errorf("%s: got %+v, want %+v", name, got, tc.want)
				}
			}
		})
	}
}

func authenticateSource(secret []byte, auth string) (int64, string, string) {
	return (&app{secret: secret}).authenticate(auth)
}

func TestSignatureVerifiedOnEveryCall(t *testing.T) {
	auth := token(header, claims)
	for i := 0; i < 10; i++ {
		if got := call(authenticateSource, secret, auth); got != (result{1, "fixture", ""}) {
			t.Fatal(got)
		}
		if got := call(authenticateSource, []byte("different secret"), auth); got != (result{err: invalid}) {
			t.Fatal(got)
		}
	}
}

func TestAuthenticationConcurrent(t *testing.T) {
	cs := cases()
	var wg sync.WaitGroup
	for n := 0; n < 8; n++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 3; i++ {
				for _, tc := range cs {
					if got := call(authenticateSource, secret, tc.auth); got != tc.want {
						t.Errorf("%s: %+v", tc.name, got)
					}
				}
			}
		}()
	}
	wg.Wait()
}

func TestClockClaims(t *testing.T) {
	now := time.Now().Unix()
	for _, tc := range []struct {
		delta int64
		err   string
	}{{-3600, invalid}, {3600, ""}} {
		auth := token(header, fmt.Sprintf(`{"sub":"1","username":"fixture","exp":%d}`, now+tc.delta))
		for _, fn := range []authFunc{authenticateSource} {
			if got := call(fn, secret, auth); got.err != tc.err {
				t.Fatalf("expiry delta %d: %+v", tc.delta, got)
			}
		}
	}
}

func TestClockRecheckedOnEveryCall(t *testing.T) {
	boundary := time.Now().Add(250 * time.Millisecond)
	seconds := float64(boundary.UnixNano()) / 1e9
	expires := token(header, fmt.Sprintf(`{"sub":"1","username":"fixture","exp":%.6f}`, seconds))
	starts := token(header, fmt.Sprintf(`{"sub":"1","username":"fixture","exp":4102444800,"nbf":%.6f}`, seconds))
	for _, fn := range []authFunc{authenticateSource} {
		if got := call(fn, secret, expires); got != (result{1, "fixture", ""}) {
			t.Fatalf("before exp: %+v", got)
		}
		if got := call(fn, secret, starts); got != (result{err: invalid}) {
			t.Fatalf("before nbf: %+v", got)
		}
	}
	time.Sleep(time.Until(boundary) + time.Millisecond)
	for _, fn := range []authFunc{authenticateSource} {
		if got := call(fn, secret, expires); got != (result{err: invalid}) {
			t.Fatalf("after exp: %+v", got)
		}
		if got := call(fn, secret, starts); got != (result{1, "fixture", ""}) {
			t.Fatalf("after nbf: %+v", got)
		}
	}
}
