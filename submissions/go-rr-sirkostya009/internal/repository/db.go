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

// DB is the SQLite database: a pool of reader connections and one writer.
// WAL lets the readers run beside the writer; SQLite allows one writer at a
// time anyway, so the writer is a single connection behind a mutex rather
// than several connections fighting over the lock.
//
// The pool is a plain channel rather than sqlitex.Pool, whose Get starts a
// goroutine per call to watch the context.
type DB struct {
	readers chan *sqlite.Conn
	all     []*sqlite.Conn

	wmu sync.Mutex
	w   *sqlite.Conn
}

// pragmas every connection runs. synchronous=NORMAL in WAL mode commits
// durably enough for rule 6; mmap and the page cache are SQLite's own.
var pragmas = []string{
	`PRAGMA synchronous = NORMAL`,
	`PRAGMA temp_store = MEMORY`,
	`PRAGMA mmap_size = 1073741824`,
	`PRAGMA cache_size = -32768`,
}

// Open opens path with readers reader connections and one writer, and
// prepares every statement so a broken query fails here, not on a request.
func Open(path string, readers int) (*DB, error) {
	db := &DB{readers: make(chan *sqlite.Conn, readers)}
	w, err := sqlite.OpenConn(path, sqlite.SQLITE_OPEN_READWRITE|sqlite.SQLITE_OPEN_WAL|sqlite.SQLITE_OPEN_NOMUTEX)
	if err != nil {
		return nil, err
	}
	db.w = w
	if err := setup(w, sqlInsert, sqlLike, sqlExists); err != nil {
		db.Close()
		return nil, err
	}
	for range readers {
		c, err := sqlite.OpenConn(path, sqlite.SQLITE_OPEN_READONLY|sqlite.SQLITE_OPEN_NOMUTEX)
		if err != nil {
			db.Close()
			return nil, err
		}
		db.all = append(db.all, c)
		if err := setup(c, sqlFeed, sqlGet, sqlPing); err != nil {
			db.Close()
			return nil, err
		}
		db.readers <- c
	}
	return db, nil
}

// setup runs the pragmas on c and prepares queries, which c then keeps.
func setup(c *sqlite.Conn, queries ...string) error {
	for _, p := range pragmas {
		if err := sqlitex.ExecTransient(c, p, nil); err != nil {
			return err
		}
	}
	for _, q := range queries {
		if _, err := c.Prepare(q); err != nil {
			return err
		}
	}
	return nil
}

func (db *DB) reader(ctx context.Context) (*sqlite.Conn, error) {
	select {
	case c := <-db.readers:
		return c, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (db *DB) release(c *sqlite.Conn) { db.readers <- c }

func (db *DB) writer(ctx context.Context) (*sqlite.Conn, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	db.wmu.Lock()
	return db.w, nil
}

func (db *DB) releaseWriter() { db.wmu.Unlock() }

// Ping runs SELECT 1 on a reader.
func (db *DB) Ping(ctx context.Context) error {
	c, err := db.reader(ctx)
	if err != nil {
		return err
	}
	defer db.release(c)
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

// Close closes every connection and the statements it kept. Only call it
// once no request is running.
func (db *DB) Close() error {
	var errs []error
	for _, c := range db.all {
		errs = append(errs, c.Close())
	}
	if db.w != nil {
		errs = append(errs, db.w.Close())
	}
	return errors.Join(errs...)
}
