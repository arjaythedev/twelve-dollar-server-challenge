// A feed API for the $12 server challenge: one epoll event loop in Go, SQLite through cgo.
//
// Config (SPEC.md): SQLITE_PATH, JWT_SECRET, HOST, PORT.
package main

import (
	"log"
	"os"
	"runtime/debug"
)

func main() {
	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		log.Fatal("SQLITE_PATH and JWT_SECRET are required")
	}
	host, port := getenv("HOST", "127.0.0.1"), getenv("PORT", "3000")

	// Request handling allocates almost nothing, so the heap stays tiny; collect it less often.
	debug.SetGCPercent(400)

	if err := sqliteSetup(); err != nil {
		log.Fatal(err)
	}
	a, err := newAPI(path, []byte(secret))
	if err != nil {
		log.Fatal(err)
	}
	lfd, err := listen(host, port)
	if err != nil {
		log.Fatal(err)
	}
	l, err := newLoop(lfd, a)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("listening on %s:%s", host, port)
	l.run()
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
