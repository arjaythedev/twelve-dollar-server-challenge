package main

import (
	"log"
	"os"
)

func must[T any](val T, err error) T {
	if err != nil {
		log.Fatal(err.Error())
	}

	return val
}

func env(key string, def string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}

	return def
}
