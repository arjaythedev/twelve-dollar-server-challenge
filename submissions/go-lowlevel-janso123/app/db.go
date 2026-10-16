package main

import (
	"context"
	"database/sql"
	"fmt"
	"io"
	"net/url"
	"path/filepath"
)

func openSQLite(path string, diagnostics io.Writer) (*sql.DB, *sql.Conn, error) {
	path, err := filepath.Abs(path)
	if err != nil {
		return nil, nil, err
	}
	dsn := (&url.URL{Scheme: "file", Path: path}).String() + "?mode=rw"
	db, err := sql.Open(sqliteDriver, dsn)
	if err != nil {
		return nil, nil, err
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	conn, err := db.Conn(context.Background())
	if err != nil {
		db.Close()
		return nil, nil, err
	}
	fail := func(err error) (*sql.DB, *sql.Conn, error) {
		conn.Close()
		db.Close()
		return nil, nil, err
	}
	pragmas := []struct{ name, value string }{
		{"locking_mode", "NORMAL"},
		{"journal_mode", "WAL"},
		{"synchronous", "NORMAL"},
		{"foreign_keys", "ON"},
		{"busy_timeout", "0"},
		{"cache_size", "-8192"},
		{"mmap_size", "268435456"},
	}
	for _, p := range pragmas {
		if _, err := conn.ExecContext(context.Background(), "PRAGMA "+p.name+"="+p.value); err != nil {
			return fail(fmt.Errorf("initialize %s: %w", p.name, err))
		}
	}
	return db, conn, nil
}
