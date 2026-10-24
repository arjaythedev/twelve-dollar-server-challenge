package main

import (
	"log"
	"os"
	"os/signal"
	"runtime/debug"
	"runtime/pprof"
	"syscall"
	"time"
)

func main() {
	// Steady state allocates nothing; set a high GC target and a safe memory limit for the 2 GiB droplet.
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(1200 << 20)

	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		log.Fatal("SQLITE_PATH and JWT_SECRET are required")
	}
	d, err := openDB(path)
	if err != nil {
		log.Fatal(err)
	}
	if pgoPath := os.Getenv("PGO_RECORD"); pgoPath != "" {
		if f, err := os.Create(pgoPath); err == nil {
			_ = pprof.StartCPUProfile(f)
			sigCh := make(chan os.Signal, 1)
			signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
			go func() {
				if dStr := os.Getenv("PGO_DURATION"); dStr != "" {
					if dur, err := time.ParseDuration(dStr); err == nil {
						select {
						case <-sigCh:
						case <-time.After(dur):
						}
					} else {
						<-sigCh
					}
				} else {
					<-sigCh
				}
				pprof.StopCPUProfile()
				_ = f.Close()
				os.Exit(0)
			}()
		}
	}

	log.Fatal(serve(env("HOST", "127.0.0.1"), env("PORT", "3000"), d, secret))
}

func env(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
