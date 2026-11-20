package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"os"
	"runtime"
	"testing"
	"time"
)

// BENCH_DB must be a disposable copy of seed/feed.db. Nothing reads token files
// or marshals JSON: the synthetic fixture is signed exactly like a client JWT.
func BenchmarkEndpoints(b *testing.B) {
	path := os.Getenv("BENCH_DB")
	if path == "" {
		b.Skip("set BENCH_DB to a disposable seed database")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := dbOpen(path); err != nil {
		b.Fatal(err)
	}
	const secret = "twelve-dollar-challenge"
	header := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	payload := appendInt([]byte(`{"sub":"1","username":"golden_ember_1","exp":`), time.Now().Unix()+3600)
	payload = append(payload, '}')
	token := header + "." + base64.RawURLEncoding.EncodeToString(payload)
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write([]byte(token))
	auth := []byte("Bearer " + token + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil)))
	for _, name := range []string{"health", "feed", "post", "create", "like"} {
		b.Run(name, func(b *testing.B) {
			a := newApp([]byte(secret))
			a.now = time.Now().Unix()
			id, _, ok := dbInsertPost(1, []byte("benchmark fixture"))
			if !ok || !dbCommit() {
				b.Fatal("fixture insert failed")
			}
			postPath := appendInt([]byte("/posts/"), id)
			r := request{method: []byte("GET"), auth: auth, hasAuth: true}
			want := []byte("HTTP/1.1 200 ")
			switch name {
			case "health":
				r.path = []byte("/health")
			case "feed":
				r.path = []byte("/feed")
			case "post":
				r.path = postPath
			case "create":
				r.method, r.path, r.body = []byte("POST"), []byte("/posts"), []byte(`{"body":"synthetic benchmark post"}`)
				want = []byte("HTTP/1.1 201 ")
			case "like":
				r.method, r.path = []byte("POST"), append(postPath, "/like"...)
				if dbLike(1, id) != 2 || !dbCommit() {
					b.Fatal("fixture like failed")
				}
			}
			buf := make([]byte, 0, 65536)
			execute := func() {
				buf = a.handle(&r, buf[:0])
				if !dbCommit() || !bytes.HasPrefix(buf, want) {
					b.Fatalf("request failed: %s", buf)
				}
			}
			execute() // warm scratch capacity
			if allocs := testing.AllocsPerRun(10, execute); allocs != 0 {
				b.Fatalf("endpoint allocates: %g", allocs)
			}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				execute()
			}
		})
	}
}
