package main

/*
#cgo LDFLAGS: -lsqlite3
#include <stdlib.h>
#include <sqlite3.h>

// exec_sql runs a statement and reports the error message (if any).
static int exec_sql(sqlite3 *db, const char *sql, char **errmsg) {
	return sqlite3_exec(db, sql, NULL, NULL, errmsg);
}

// bind_text_transient binds a text parameter, copying it immediately, so the
// Go-owned bytes never outlive the call. A NULL pointer (empty input) binds an
// empty string rather than SQL NULL.
static int bind_text_transient(sqlite3_stmt *st, int idx, const char *p, int n) {
	if (p == NULL) {
		static const char empty = 0;
		p = &empty;
	}
	return sqlite3_bind_text(st, idx, p, n, SQLITE_TRANSIENT);
}
*/
import "C"

import (
	"errors"
	"strconv"
	"sync"
	"time"
	"unsafe"
)

// SQLite result / open codes, as plain Go constants so we never depend on how
// cgo exposes the C macros.
const (
	sqliteOK   = 0
	sqliteRow  = 100
	sqliteDone = 101

	sqliteOpenReadWrite = 0x00000002
	sqliteOpenNoMutex   = 0x00008000
)

const (
	sqlFeed = `SELECT p.id, p.body, p.created_at, u.username, ` +
		`(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count ` +
		`FROM posts p JOIN users u ON u.id = p.user_id ` +
		`ORDER BY p.created_at DESC, p.id DESC LIMIT 20`

	sqlPostByID = `SELECT p.id, p.body, p.created_at, u.username, ` +
		`(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count ` +
		`FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`

	sqlPostExists = `SELECT 1 FROM posts WHERE id = ?`

	sqlHealth = `SELECT 1`

	sqlInsertPost = `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`

	// Inserts only if the post exists; 0 changes means "already liked" or "no such post".
	sqlInsertLike = `INSERT INTO likes (user_id, post_id) ` +
		`SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ` +
		`ON CONFLICT (user_id, post_id) DO NOTHING`
)

type sqliteConn struct {
	db *C.sqlite3

	feed       *C.sqlite3_stmt
	postByID   *C.sqlite3_stmt
	postExists *C.sqlite3_stmt
	health     *C.sqlite3_stmt
	insertPost *C.sqlite3_stmt
	insertLike *C.sqlite3_stmt
}

// DB owns one write connection (guarded by a mutex) and a small pool of read
// connections. All connections are opened with SQLITE_OPEN_NOMUTEX because each
// one is only ever touched by a single goroutine at a time.
type DB struct {
	reads chan *sqliteConn
	write *sqliteConn
	ckpt  *sqliteConn
	wmu   sync.Mutex
}

// Open opens the database and prepares the read pool.
func Open(path string, reads int) (*DB, error) {
	w, err := openConn(path)
	if err != nil {
		return nil, err
	}
	db := &DB{write: w, reads: make(chan *sqliteConn, reads)}
	ck, err := openConn(path)
	if err != nil {
		w.Close()
		return nil, err
	}
	db.ckpt = ck
	for i := 0; i < reads; i++ {
		c, err := openConn(path)
		if err != nil {
			db.Close()
			return nil, err
		}
		db.reads <- c
	}
	return db, nil
}

func openConn(path string) (*sqliteConn, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	var db *C.sqlite3
	flags := C.int(sqliteOpenReadWrite | sqliteOpenNoMutex)
	if rc := int(C.sqlite3_open_v2(cpath, &db, flags, nil)); rc != sqliteOK {
		msg := "sqlite3_open_v2 failed"
		if db != nil {
			msg = C.GoString(C.sqlite3_errmsg(db))
			C.sqlite3_close_v2(db)
		}
		return nil, errors.New(msg)
	}

	c := &sqliteConn{db: db}
	C.sqlite3_busy_timeout(db, 5000)

	// Durability contract (rule 6): WAL + NORMAL. mmap lets reads come straight
	// from the OS page cache with no copies; temp_store keeps sorting in RAM.
	for _, p := range []string{
		"PRAGMA journal_mode=WAL",
		"PRAGMA synchronous=NORMAL",
		"PRAGMA busy_timeout=5000",
		"PRAGMA mmap_size=1073741824",
		"PRAGMA cache_size=-4096",
		"PRAGMA temp_store=MEMORY",
		"PRAGMA wal_autocheckpoint=4000",
	} {
		if err := c.exec(p); err != nil {
			c.Close()
			return nil, err
		}
	}

	var err error
	if c.feed, err = c.prepare(sqlFeed); err != nil {
		c.Close()
		return nil, err
	}
	if c.postByID, err = c.prepare(sqlPostByID); err != nil {
		c.Close()
		return nil, err
	}
	if c.postExists, err = c.prepare(sqlPostExists); err != nil {
		c.Close()
		return nil, err
	}
	if c.health, err = c.prepare(sqlHealth); err != nil {
		c.Close()
		return nil, err
	}
	if c.insertPost, err = c.prepare(sqlInsertPost); err != nil {
		c.Close()
		return nil, err
	}
	if c.insertLike, err = c.prepare(sqlInsertLike); err != nil {
		c.Close()
		return nil, err
	}
	return c, nil
}

func (c *sqliteConn) exec(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	var errmsg *C.char
	if rc := int(C.exec_sql(c.db, csql, &errmsg)); rc != sqliteOK {
		if errmsg != nil {
			e := errors.New(C.GoString(errmsg))
			C.sqlite3_free(unsafe.Pointer(errmsg))
			return e
		}
		return errors.New(C.GoString(C.sqlite3_errmsg(c.db)))
	}
	return nil
}

func (c *sqliteConn) prepare(sql string) (*C.sqlite3_stmt, error) {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	var st *C.sqlite3_stmt
	if rc := int(C.sqlite3_prepare_v2(c.db, csql, C.int(len(sql)), &st, nil)); rc != sqliteOK {
		return nil, errors.New(C.GoString(C.sqlite3_errmsg(c.db)))
	}
	return st, nil
}

func (c *sqliteConn) err() error { return errors.New(C.GoString(C.sqlite3_errmsg(c.db))) }

// Close finalizes every statement and closes the connection.
func (c *sqliteConn) Close() {
	if c == nil || c.db == nil {
		return
	}
	for _, st := range []*C.sqlite3_stmt{c.feed, c.postByID, c.postExists, c.health, c.insertPost, c.insertLike} {
		if st != nil {
			C.sqlite3_finalize(st)
		}
	}
	C.sqlite3_close_v2(c.db)
	c.db = nil
}

// Close closes every connection.
func (db *DB) Close() {
	if db.reads != nil {
		close(db.reads)
		for c := range db.reads {
			c.Close()
		}
		db.reads = nil
	}
	if db.write != nil {
		db.write.Close()
		db.write = nil
	}
	if db.ckpt != nil {
		db.ckpt.Close()
		db.ckpt = nil
	}
}

// Checkpoint runs a PASSIVE WAL checkpoint on a connection that is not used for
// requests, so copying WAL pages back into the database never blocks a writer.
func (db *DB) Checkpoint() { db.ckpt.exec("PRAGMA wal_checkpoint(PASSIVE)") }

// CheckpointLoop checkpoints periodically. wal_autocheckpoint is left set as a
// backstop, but this keeps the WAL small without putting a checkpoint's fsync in
// the path of a write request.
func (db *DB) CheckpointLoop(interval time.Duration) {
	t := time.NewTicker(interval)
	defer t.Stop()
	for range t.C {
		db.Checkpoint()
	}
}

func bindInt64(st *C.sqlite3_stmt, idx int, v int64) int {
	return int(C.sqlite3_bind_int64(st, C.int(idx), C.sqlite3_int64(v)))
}

func bindText(st *C.sqlite3_stmt, idx int, v []byte) int {
	var p *C.char
	if len(v) > 0 {
		p = (*C.char)(unsafe.Pointer(unsafe.SliceData(v)))
	}
	return int(C.bind_text_transient(st, C.int(idx), p, C.int(len(v))))
}

func reset(st *C.sqlite3_stmt) { C.sqlite3_reset(st) }

func colInt64(st *C.sqlite3_stmt, idx int) int64 {
	return int64(C.sqlite3_column_int64(st, C.int(idx)))
}

// colText returns a slice aliasing SQLite's internal buffer. It is only valid
// until the next step/reset, so callers must copy it immediately.
func colText(st *C.sqlite3_stmt, idx int) []byte {
	p := C.sqlite3_column_text(st, C.int(idx))
	if p == nil {
		return nil
	}
	n := int(C.sqlite3_column_bytes(st, C.int(idx)))
	if n == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), n)
}

// AppendFeed appends {"posts":[ ... 20 posts ... ]} to dst.
func (db *DB) AppendFeed(dst []byte) ([]byte, error) {
	c := <-db.reads
	st := c.feed
	rc := int(C.sqlite3_step(st))
	dst = append(dst, `{"posts":[`...)
	first := true
	for rc == sqliteRow {
		if !first {
			dst = append(dst, ',')
		}
		first = false
		dst = appendPostObject(dst, st)
		rc = int(C.sqlite3_step(st))
	}
	reset(st)
	db.reads <- c
	if rc != sqliteDone {
		return nil, c.err()
	}
	return append(dst, ']', '}'), nil
}

// AppendGetPost appends the post object to dst; found reports whether the row existed.
func (db *DB) AppendGetPost(dst []byte, id int64) (out []byte, found bool, err error) {
	c := <-db.reads
	st := c.postByID
	if rc := bindInt64(st, 1, id); rc != sqliteOK {
		reset(st)
		db.reads <- c
		return nil, false, c.err()
	}
	rc := int(C.sqlite3_step(st))
	switch rc {
	case sqliteRow:
		dst = append(dst, `{"post":`...)
		dst = appendPostObject(dst, st)
		dst = append(dst, '}')
		found = true
	case sqliteDone:
	default:
		reset(st)
		db.reads <- c
		return nil, false, c.err()
	}
	reset(st)
	db.reads <- c
	return dst, found, nil
}

func (db *DB) Health() error {
	c := <-db.reads
	st := c.health
	rc := int(C.sqlite3_step(st))
	reset(st)
	db.reads <- c
	if rc != sqliteRow {
		if rc == sqliteDone {
			return nil
		}
		return c.err()
	}
	return nil
}

// AppendCreatePost inserts the post, waits for the commit, and appends the full
// {"post":{...}} response body to dst.
func (db *DB) AppendCreatePost(dst []byte, userID int64, body, username []byte) ([]byte, error) {
	db.wmu.Lock()
	c := db.write
	st := c.insertPost
	if rc := bindInt64(st, 1, userID); rc != sqliteOK {
		reset(st)
		db.wmu.Unlock()
		return nil, c.err()
	}
	if rc := bindText(st, 2, body); rc != sqliteOK {
		reset(st)
		db.wmu.Unlock()
		return nil, c.err()
	}
	rc := int(C.sqlite3_step(st))
	if rc != sqliteRow {
		reset(st)
		db.wmu.Unlock()
		return nil, c.err()
	}
	id := colInt64(st, 0)
	createdAt := colText(st, 1)

	dst = append(dst, `{"post":{"id":`...)
	dst = strconv.AppendInt(dst, id, 10)
	dst = append(dst, `,"body":`...)
	dst = appendJSONString(dst, body)
	dst = append(dst, `,"created_at":`...)
	dst = appendJSONString(dst, createdAt)
	dst = append(dst, `,"author":`...)
	dst = appendJSONString(dst, username)
	dst = append(dst, `,"like_count":0}}`...)

	// Step to SQLITE_DONE so the write is committed before we respond (rule 6).
	for int(C.sqlite3_step(st)) == sqliteRow {
	}
	reset(st)
	db.wmu.Unlock()
	return dst, nil
}

// InsertLike returns inserted=true for a fresh like. When nothing was inserted it
// reports whether the post exists (already liked) or not (404).
func (db *DB) InsertLike(userID, postID int64) (inserted, postExists bool, err error) {
	db.wmu.Lock()
	c := db.write
	st := c.insertLike
	if rc := bindInt64(st, 1, userID); rc != sqliteOK {
		reset(st)
		db.wmu.Unlock()
		return false, false, c.err()
	}
	if rc := bindInt64(st, 2, postID); rc != sqliteOK {
		reset(st)
		db.wmu.Unlock()
		return false, false, c.err()
	}
	rc := int(C.sqlite3_step(st))
	if rc != sqliteDone {
		reset(st)
		db.wmu.Unlock()
		return false, false, c.err()
	}
	changes := int64(C.sqlite3_changes(c.db))
	reset(st)
	if changes > 0 {
		db.wmu.Unlock()
		return true, true, nil
	}

	es := c.postExists
	if rc := bindInt64(es, 1, postID); rc != sqliteOK {
		reset(es)
		db.wmu.Unlock()
		return false, false, c.err()
	}
	rc = int(C.sqlite3_step(es))
	switch rc {
	case sqliteRow:
		postExists = true
	case sqliteDone:
	default:
		reset(es)
		db.wmu.Unlock()
		return false, false, c.err()
	}
	reset(es)
	db.wmu.Unlock()
	return false, postExists, nil
}

func appendPostObject(dst []byte, st *C.sqlite3_stmt) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, colInt64(st, 0), 10)
	dst = append(dst, `,"body":`...)
	dst = appendJSONString(dst, colText(st, 1))
	dst = append(dst, `,"created_at":`...)
	dst = appendJSONString(dst, colText(st, 2))
	dst = append(dst, `,"author":`...)
	dst = appendJSONString(dst, colText(st, 3))
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, colInt64(st, 4), 10)
	return append(dst, '}')
}
