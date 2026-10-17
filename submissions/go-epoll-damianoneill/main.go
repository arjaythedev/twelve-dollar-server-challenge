package main

import (
	"log"
	"os"
	"time"
)

func main() {
	path, secret := os.Getenv("SQLITE_PATH"), os.Getenv("JWT_SECRET")
	if path == "" || secret == "" {
		log.Fatal("SQLITE_PATH and JWT_SECRET are required")
	}
	jwtSecret = []byte(secret)
	d, err := openDB(path)
	if err != nil {
		log.Fatal(err)
	}
	log.Fatal(serve(env("HOST", "127.0.0.1"), env("PORT", "3000"), &app{db: d, started: time.Now()}))
}

func env(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
