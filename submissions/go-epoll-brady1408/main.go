// Command server is the go-epoll-brady1408 submission to the $12 Server
// Challenge: one epoll event loop, one SQLite connection, group commit.
package main

import (
	"log"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"
	"strconv"
	"syscall"
)

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func main() {
	// The loop keeps one P for itself (see loop.wait); the checkpointer, the
	// garbage collector and signal delivery share the other, so none of them
	// waits on sysmon to notice a blocked P.
	runtime.GOMAXPROCS(2)
	debug.SetMemoryLimit(1 << 30)
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	sqlitePath := os.Getenv("SQLITE_PATH")
	secret := os.Getenv("JWT_SECRET")
	if sqlitePath == "" || secret == "" {
		log.Fatal("SQLITE_PATH and JWT_SECRET must be set")
	}
	host := envOr("HOST", "127.0.0.1")
	port, err := strconv.Atoi(envOr("PORT", "3000"))
	if err != nil {
		log.Fatalf("PORT: %v", err)
	}

	db, err := openLoopConn(sqlitePath)
	if err != nil {
		log.Fatalf("open %s: %v", sqlitePath, err)
	}
	cpdb, err := openCheckpointConn(sqlitePath)
	if err != nil {
		log.Fatalf("open checkpoint connection: %v", err)
	}
	srv, err := newServer(db, []byte(secret))
	if err != nil {
		log.Fatalf("prepare statements: %v", err)
	}
	l, err := newLoop(srv, host, port)
	if err != nil {
		log.Fatalf("listen %s:%d: %v", host, port, err)
	}

	reqs, done := make(chan struct{}, 1), make(chan struct{}, 1)
	stop, exited := make(chan struct{}), make(chan struct{})
	go runCheckpointer(cpdb, reqs, done, l.wake, stop, exited)
	l.ckptReq, l.ckptDone = reqs, done
	l.walPath = sqlitePath + "-wal"

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	go func() { <-sigs; l.stop() }()

	log.Printf("listening on %s:%d (budget %d connections)", host, l.port(), l.budget)
	l.run()
	close(stop)
	<-exited
}
