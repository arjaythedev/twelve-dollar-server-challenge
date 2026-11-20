package httpserver

import (
	"os"
)

// Config chứa toàn bộ tham số môi trường theo SPEC.md
type Config struct {
	DBPath    string
	JWTSecret []byte
	Host      string
	Port      string
}

func LoadConfig() Config {
	dbPath := os.Getenv("SQLITE_PATH")
	if dbPath == "" {
		dbPath = "../../seed/feed.db"
	}
	secret := os.Getenv("JWT_SECRET")
	if secret == "" {
		secret = "twelve-dollar-challenge"
	}
	host := os.Getenv("HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "3000"
	}

	return Config{
		DBPath:    dbPath,
		JWTSecret: []byte(secret),
		Host:      host,
		Port:      port,
	}
}
