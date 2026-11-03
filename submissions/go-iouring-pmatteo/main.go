package main

// Startup: read the configuration, then hand over to the io_uring loop in loop_linux.go.
//
// Linux only, and deliberately: the whole design is registered descriptors, multishot operations
// and provided buffer rings, none of which has a portable equivalent worth keeping a second code
// path for.

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

type config struct {
	host       string
	port       string
	sqlitePath string
	secret     string

	// Loop knobs. The right value for each is measured on the target box rather than derived, so
	// none of them is written as a literal in the serving code.
	rings      int
	slots      uint32 // registered file slots per ring; 0 means derive from RLIMIT_NOFILE
	bufCount   uint16
	bufSize    int
	statsEvery uint32
	tickMs     int
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "fatal:", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := loadConfig()
	if err != nil {
		return err
	}
	// build.sh runs the binary once in this mode to produce the profiles it then rebuilds
	// against. It needs no database and no port, so it is checked before the config is.
	if len(os.Args) > 1 && os.Args[1] == "train" {
		return runTraining(cfg)
	}
	if cfg.sqlitePath == "" {
		return fmt.Errorf("SQLITE_PATH is not set")
	}
	return serve(cfg)
}

func loadConfig() (config, error) {
	cfg := config{
		host:       env("HOST", "0.0.0.0"),
		port:       env("PORT", "3000"),
		sqlitePath: env("SQLITE_PATH", ""),
		secret:     env("JWT_SECRET", "twelve-dollar-challenge"),

		rings:      envInt("RINGS", 8),
		slots:      uint32(envInt("SLOTS", 0)),
		bufCount:   uint16(envInt("BUF_COUNT", 1024)),
		bufSize:    envInt("BUF_SIZE", 4096),
		statsEvery: uint32(envInt("STATS_EVERY", 0)),
		tickMs:     envInt("TICK_MS", 1000),
	}
	if cfg.rings < 1 {
		return cfg, fmt.Errorf("RINGS must be at least 1, got %d", cfg.rings)
	}
	return cfg, nil
}

// newServerState opens the database and builds the request handler.
//
// The caller must already own the thread this runs on. SQLITE_THREADSAFE=0 means the database may
// only ever be entered from one thread, and that thread is the one that opens it.
func newServerState(cfg config) (*server, error) {
	d, err := openDB(cfg.sqlitePath)
	if err != nil {
		return nil, err
	}
	s := &server{
		db:         d,
		ver:        newVerifier([]byte(cfg.secret)),
		started:    time.Now(),
		trim:       make([]byte, 0, 4096),
		likeCounts: make([]int32, maxFeedSpan),
	}
	return s, nil
}

func env(key, def string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
		fmt.Fprintf(os.Stderr, "ignoring %s=%q: not an integer\n", key, v)
	}
	return def
}
