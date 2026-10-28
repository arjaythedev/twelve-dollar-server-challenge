package main

import (
	"log"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"time"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	path, secret := mustEnv("SQLITE_PATH"), mustEnv("JWT_SECRET")
	host := envOr("HOST", "127.0.0.1")
	port, err := strconv.Atoi(envOr("PORT", "3000"))
	if err != nil {
		log.Fatalf("PORT: %v", err)
	}
	worker := os.Getenv(workerEnv)
	log.SetPrefix("[w" + envOr(workerEnv, "0") + "] ")

	// Steady state allocates nothing, so the collector only runs at startup; the limit is a
	// backstop that keeps the heap well inside the 2 GB box if that ever stops being true.
	debug.SetGCPercent(400)
	debug.SetMemoryLimit(384 << 20)

	d, err := openDB(path, []string{
		"PRAGMA journal_mode = WAL",
		"PRAGMA synchronous = NORMAL",
		"PRAGMA wal_autocheckpoint = 0",
		"PRAGMA mmap_size = 1073741824",
		"PRAGMA cache_size = -16384",
		"PRAGMA temp_store = MEMORY",
	})
	if err != nil {
		log.Fatal(err)
	}
	if err := d.prepareAll(); err != nil {
		log.Fatal(err)
	}

	runtime.LockOSThread()
	s, err := newServer(host, port, newApp(d, secret))
	if err != nil {
		log.Fatal(err)
	}
	if worker == "" {
		go checkpointer(path)
		if s.ctrl, err = newController(s); err != nil {
			log.Fatal(err)
		}
	} else {
		s.rep = &reporter{f: os.NewFile(3, "report")}
		s.rep.send(msgReady)
	}
	s.run()
}

// checkpointer moves WAL frames into the database file on its own connection and thread, so
// the fsync a checkpoint needs never stalls an event loop (they run with autocheckpoint off).
func checkpointer(path string) {
	cp, err := openDB(path, []string{"PRAGMA synchronous = NORMAL", "PRAGMA busy_timeout = 2000"})
	if err != nil {
		log.Fatalf("checkpointer: %v", err)
	}
	for range time.Tick(time.Second) {
		cp.checkpoint()
	}
}

func mustEnv(k string) string {
	v := os.Getenv(k)
	if v == "" {
		log.Fatalf("%s is required", k)
	}
	return v
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
