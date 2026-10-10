package repository

import (
	"context"
	"errors"
	"sync"

	"crawshaw.io/sqlite"
	"crawshaw.io/sqlite/sqlitex"
)

// Pinger is what the health check asks.
type Pinger interface {
	Ping(ctx context.Context) error
}

// DB is the SQLite database: one connection behind a mutex, in exclusive
// locking mode. On one core a pool of readers never runs in parallel anyway,
// and exclusive mode keeps the WAL index in heap memory instead of the -shm
// file, so a read transaction takes no file locks. Nothing else can open the
// database while the server runs.
type DB struct {
	mu sync.Mutex
	c  *sqlite.Conn
}

// Open opens path and prepares every statement so a broken query fails
// here, not on a request.
func Open(path string) (*DB, error) {
	c, err := sqlite.OpenConn(path, sqlite.SQLITE_OPEN_READWRITE|sqlite.SQLITE_OPEN_NOMUTEX)
	if err != nil {
		return nil, err
	}
	db := &DB{c: c}
	for _, pragma := range []string{
		`PRAGMA locking_mode = EXCLUSIVE`,
		`PRAGMA journal_mode = WAL`,
		`PRAGMA synchronous = NORMAL`,
		`PRAGMA temp_store = MEMORY`,
		`PRAGMA mmap_size = 1073741824`,
		`PRAGMA cache_size = -32768`,
	} {
		if err := sqlitex.ExecTransient(c, pragma, nil); err != nil {
			db.Close()
			return nil, err
		}
	}
	for _, q := range []string{sqlFeed, sqlGet, sqlPing, sqlInsert, sqlLike, sqlExists} {
		if _, err := c.Prepare(q); err != nil {
			db.Close()
			return nil, err
		}
	}
	return db, nil
}

func (db *DB) acquire(ctx context.Context) (*sqlite.Conn, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	db.mu.Lock()
	return db.c, nil
}

func (db *DB) release() { db.mu.Unlock() }

// Ping runs SELECT 1 on a reader.
func (db *DB) Ping(ctx context.Context) error {
	c, err := db.acquire(ctx)
	if err != nil {
		return err
	}
	defer db.release()
	s, err := c.Prepare(sqlPing)
	if err != nil {
		return err
	}
	defer s.Reset()
	row, err := s.Step()
	if err != nil {
		return err
	}
	if !row {
		return errors.New("SELECT 1 returned no row")
	}
	return nil
}

// Close closes the connection and the statements it kept. Only call it
// once no request is running.
func (db *DB) Close() error { return db.c.Close() }
