package config

import (
	"net"
	"os"
	"runtime"
)

type Config struct {
	Addr       string
	SQLitePath string
	JWTSecret  string
	// Readers is how many read connections the pool keeps.
	Readers int
}

func Parse() Config {
	return Config{
		Addr:       net.JoinHostPort(env("HOST", "127.0.0.1"), env("PORT", "3000")),
		SQLitePath: env("SQLITE_PATH", "feed.db"),
		JWTSecret:  env("JWT_SECRET", "twelve-dollar-challenge"),
		Readers:    max(4, 2*runtime.GOMAXPROCS(0)),
	}
}

func env(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}
