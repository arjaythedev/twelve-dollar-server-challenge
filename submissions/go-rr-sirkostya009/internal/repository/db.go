package repository

import (
	"context"
	"errors"
	"sync"

	"feed/internal/sqlite"
)

// DB is the SQLite database: a pool of reader connections and one writer.
// WAL lets the readers run beside the writer; SQLite allows one writer at a
// time anyway, so the writer is a single connection behind a mutex rather
// than several connections fighting over the lock.
type DB struct {
	readers chan *readConn
	all     []*readConn

	wmu sync.Mutex
	w   *sqlite.Conn
	ws  writeStmts
}

type readConn struct {
	c     *sqlite.Conn
	stmts readStmts
}

// pragmas every connection runs. synchronous=NORMAL in WAL mode commits
// durably enough for rule 6; mmap and the page cache are SQLite's own.
const pragmas = `PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA temp_store = MEMORY;
PRAGMA mmap_size = 1073741824;
PRAGMA cache_size = -32768;`

// Open opens path with readers reader connections and one writer.
func Open(path string, readers int) (*DB, error) {
	db := &DB{readers: make(chan *readConn, readers)}
	w, err := sqlite.Open(path, false)
	if err != nil {
		return nil, err
	}
	db.w = w
	w.BusyTimeout(5000)
	if err := w.Exec(pragmas); err != nil {
		db.Close()
		return nil, err
	}
	if db.ws, err = prepareWrite(w); err != nil {
		db.Close()
		return nil, err
	}
	for range readers {
		c, err := sqlite.Open(path, true)
		if err != nil {
			db.Close()
			return nil, err
		}
		c.BusyTimeout(5000)
		rc := &readConn{c: c}
		db.all = append(db.all, rc)
		if err := c.Exec(`PRAGMA mmap_size = 1073741824; PRAGMA cache_size = -32768; PRAGMA temp_store = MEMORY;`); err != nil {
			db.Close()
			return nil, err
		}
		if rc.stmts, err = prepareRead(c); err != nil {
			db.Close()
			return nil, err
		}
		db.readers <- rc
	}
	return db, nil
}

func (db *DB) reader(ctx context.Context) (*readConn, error) {
	select {
	case rc := <-db.readers:
		return rc, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (db *DB) release(rc *readConn) { db.readers <- rc }

func (db *DB) writer(ctx context.Context) (*writeStmts, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	db.wmu.Lock()
	return &db.ws, nil
}

func (db *DB) releaseWriter() { db.wmu.Unlock() }

// Ping runs SELECT 1 on a reader.
func (db *DB) Ping(ctx context.Context) error {
	rc, err := db.reader(ctx)
	if err != nil {
		return err
	}
	defer db.release(rc)
	s := rc.stmts.ping
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

// Close closes every connection. Only call it once no request is running.
func (db *DB) Close() error {
	var errs []error
	for _, rc := range db.all {
		for _, s := range []*sqlite.Stmt{rc.stmts.feed, rc.stmts.get, rc.stmts.ping} {
			if s != nil {
				errs = append(errs, s.Close())
			}
		}
		errs = append(errs, rc.c.Close())
	}
	if db.w != nil {
		for _, s := range []*sqlite.Stmt{db.ws.insert, db.ws.like, db.ws.exists} {
			if s != nil {
				errs = append(errs, s.Close())
			}
		}
		errs = append(errs, db.w.Close())
	}
	return errors.Join(errs...)
}
