// The $12 server challenge: Go + io_uring (Linux) / net/http (elsewhere) + SQLite B-tree reads.
package main

import (
	"log"
	"os"
	"os/signal"
	"runtime/debug"
	"syscall"
)

func main() {
	path, secret := mustEnv("SQLITE_PATH"), mustEnv("JWT_SECRET")
	host, port := envOr("HOST", "0.0.0.0"), envOr("PORT", "3000")

	signal.Ignore(syscall.SIGPIPE)
	raiseFileLimit()
	debug.SetGCPercent(400)
	debug.SetMemoryLimit(768 << 20)

	if err := dbOpen(path); err != nil {
		log.Fatalf("open %s: %v", path, err)
	}
	if err := dbCheckpointOpen(path); err != nil {
		log.Fatalf("checkpoint open %s: %v", path, err)
	}

	log.Fatal(serve(host, port, secret))
}

func raiseFileLimit() {
	var lim syscall.Rlimit
	if syscall.Getrlimit(syscall.RLIMIT_NOFILE, &lim) == nil && lim.Cur < lim.Max {
		lim.Cur = lim.Max
		_ = syscall.Setrlimit(syscall.RLIMIT_NOFILE, &lim)
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
