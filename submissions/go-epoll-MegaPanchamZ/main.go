package main

import (
	"log"
	"os"
	"runtime/debug"
)

func main() {
	// Steady state allocates nothing; set a high GC target and a safe memory limit for the 2 GiB droplet.
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(512 << 20)

	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		log.Fatal("SQLITE_PATH and JWT_SECRET are required")
	}
	d, err := openDB(path)
	if err != nil {
		log.Fatal(err)
	}
	log.Fatal(serve(env("HOST", "127.0.0.1"), env("PORT", "3000"), d, secret))
}

func env(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
