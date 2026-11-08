package main

import (
	"os"
	"runtime"
	"testing"
)

// Use the same harness in older checkouts to isolate database cost from HTTP,
// JWT verification and JSON serialization. BENCH_DB is always disposable.
func BenchmarkDatabase(b *testing.B) {
	path := os.Getenv("BENCH_DB")
	if path == "" {
		b.Skip("set BENCH_DB to a disposable seed copy")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := dbOpen(path); err != nil {
		b.Fatal(err)
	}
	id, _, ok := dbInsertPost(1, []byte("database benchmark fixture"))
	if !ok || !dbCommit() {
		b.Fatal("insert fixture")
	}
	if dbLike(1, id) != 2 || !dbCommit() {
		b.Fatal("like fixture")
	}
	for _, name := range []string{"feed", "post", "create", "repeat_like"} {
		b.Run(name, func(b *testing.B) {
			body := []byte("database benchmark post")
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				switch name {
				case "feed":
					if _, ok := dbFeed(); !ok {
						b.Fatal("feed")
					}
				case "post":
					dbPost(id)
				case "create":
					if _, _, ok := dbInsertPost(1, body); !ok {
						b.Fatal("create")
					}
				case "repeat_like":
					if dbLike(1, id) != 1 {
						b.Fatal("repeat")
					}
				}
				if !dbCommit() {
					b.Fatal("commit")
				}
			}
		})
	}
}
