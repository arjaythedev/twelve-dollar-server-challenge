package main

import (
	"sync"

	"crawshaw.io/sqlite"
	"crawshaw.io/sqlite/sqlitex"
)

// DB is a single SQLite connection shared by every request, guarded by a mutex.
//
// On a 1 vCPU box there is no parallelism to gain from a pool, and a single
// connection lets us use locking_mode=EXCLUSIVE: SQLite then keeps the WAL index
// in heap memory and never takes file locks per transaction. Without it every
// statement pays several fcntl() syscalls to begin and end its read transaction.
// It also avoids sqlitex.Pool's per-Get overhead (a context, a tracer and an
// interrupt goroutine on every call).
type DB struct {
	mu   sync.Mutex
	conn *sqlite.Conn
}

// locking_mode must be set before the first access to the WAL, so the connection
// is opened without SQLITE_OPEN_WAL (which would run "PRAGMA journal_mode" at once).
// The pragmas run one by one: sqlitex.ExecScript wraps them in a transaction, where
// synchronous can't be changed.
var pragmas = []string{
	"PRAGMA locking_mode = EXCLUSIVE",
	"PRAGMA journal_mode = WAL",
	"PRAGMA synchronous = NORMAL",
	"PRAGMA foreign_keys = ON",
	"PRAGMA temp_store = MEMORY",
	"PRAGMA mmap_size = 1073741824",
	"PRAGMA cache_size = -65536",
}

func OpenDB(path string) (*DB, error) {
	flags := sqlite.SQLITE_OPEN_READWRITE | sqlite.SQLITE_OPEN_URI | sqlite.SQLITE_OPEN_NOMUTEX
	conn, err := sqlite.OpenConn(path, flags)
	if err != nil {
		return nil, err
	}
	for _, pragma := range pragmas {
		if err = sqlitex.ExecTransient(conn, pragma, nil); err != nil {
			_ = conn.Close()
			return nil, err
		}
	}

	// take the exclusive lock now, so the first request doesn't pay for it
	if err = sqlitex.ExecTransient(conn, "SELECT 1", nil); err != nil {
		_ = conn.Close()
		return nil, err
	}

	return &DB{conn: conn}, nil
}

// Get locks the connection for the caller. Statements prepared with conn.Prep are
// cached on the connection, so each query is compiled once.
func (db *DB) Get() *sqlite.Conn {
	db.mu.Lock()
	return db.conn
}

// Put releases the connection. Statements must be reset before calling it.
func (db *DB) Put(*sqlite.Conn) { db.mu.Unlock() }

func (db *DB) Close() error { return db.conn.Close() }
