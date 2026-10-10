package config

import (
	"net"
	"os"
)

type Config struct {
	Addr       string
	SQLitePath string
	JWTSecret  string
}

func Parse() Config {
	return Config{
		Addr:       net.JoinHostPort(env("HOST", "127.0.0.1"), env("PORT", "3000")),
		SQLitePath: env("SQLITE_PATH", "feed.db"),
		JWTSecret:  env("JWT_SECRET", "twelve-dollar-challenge"),
	}
}

func env(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}
