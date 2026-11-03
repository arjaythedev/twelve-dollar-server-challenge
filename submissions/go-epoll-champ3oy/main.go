// The $12 server challenge API: a Go epoll server with its SQLite work done in C.
package main

import (
	"log"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"
	"syscall"
)

func main() {
	path, secret := mustEnv("SQLITE_PATH"), mustEnv("JWT_SECRET")
	host, port := envOr("HOST", "0.0.0.0"), envOr("PORT", "3000")

	signal.Ignore(syscall.SIGPIPE) // a client that hung up shows up as EPIPE from write
	raiseFileLimit()
	// The heap is small and almost nothing is allocated per request.
	debug.SetGCPercent(400)

	if err := dbOpen(path); err != nil {
		log.Fatalf("open %s: %v", path, err)
	}
	if err := dbCheckpointOpen(path); err != nil {
		log.Fatalf("open %s: %v", path, err)
	}

	runtime.LockOSThread()
	s, err := newServer(host, port, secret)
	if err != nil {
		log.Fatalf("listen %s:%s: %v", host, port, err)
	}
	log.Printf("listening on %s:%s (%s)", host, port, s.io.name())
	s.io.run()
}

func raiseFileLimit() {
	var lim syscall.Rlimit
	if syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim) == nil && lim.Cur < lim.Max {
		lim.Cur = lim.Max
		syscall.Setrlimit(syscall.RLIMIT_NOFILE, &lim)
	}
}

func mustEnv(k string) string {
	v := os.Getenv(k)
	if v == "" {
		log.Fatalf("%s is not set", k)
	}
	return v
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
