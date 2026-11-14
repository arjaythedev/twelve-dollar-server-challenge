//go:build cgo_sqlite && !pure_sqlite

package main

import _ "github.com/mattn/go-sqlite3"

const sqliteDriver = "sqlite3"
