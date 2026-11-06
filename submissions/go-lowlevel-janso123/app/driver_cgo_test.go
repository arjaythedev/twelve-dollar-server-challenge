//go:build cgo_sqlite && !pure_sqlite

package main

const wantDriverType = "*sqlite3.SQLiteDriver"
