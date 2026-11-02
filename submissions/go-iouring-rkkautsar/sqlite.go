// Minimal direct SQLite binding. We compile the pinned amalgamation
// (SQLITE_THREADSAFE=2) and use one connection per goroutine at a time: a
// small read pool plus one dedicated writer.
package main

/*
#cgo CFLAGS: -I${SRCDIR} -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0
#include <stdlib.h>
#include "sqlite3.h"

static int bind_text_transient(sqlite3_stmt *s, int i, const char *p, int n) {
	return sqlite3_bind_text(s, i, p, n, SQLITE_TRANSIENT);
}
*/
import "C"

import (
	"errors"
	"unsafe"
)

var emptyByte byte

type sqliteConn struct {
	h *C.sqlite3
}

type sqliteStmt struct {
	c *sqliteConn
	s *C.sqlite3_stmt
}

func openSQLite(path string) (*sqliteConn, error) {
	cs := C.CString(path)
	defer C.free(unsafe.Pointer(cs))
	var h *C.sqlite3
	flags := C.int(C.SQLITE_OPEN_READWRITE) | C.int(C.SQLITE_OPEN_CREATE) | C.int(C.SQLITE_OPEN_NOMUTEX)
	if rc := C.sqlite3_open_v2(cs, &h, flags, nil); rc != C.SQLITE_OK {
		msg := "sqlite: open failed"
		if h != nil {
			msg = C.GoString(C.sqlite3_errmsg(h))
			C.sqlite3_close(h)
		}
		return nil, errors.New(msg)
	}
	c := &sqliteConn{h: h}
	C.sqlite3_busy_timeout(h, 5000)
	if err := c.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;" +
		"PRAGMA cache_size=-16384; PRAGMA mmap_size=268435456;"); err != nil {
		c.close()
		return nil, err
	}
	return c, nil
}

func (c *sqliteConn) err() error {
	return errors.New(C.GoString(C.sqlite3_errmsg(c.h)))
}

func (c *sqliteConn) close() {
	C.sqlite3_close(c.h)
}

func (c *sqliteConn) exec(sql string) error {
	cs := C.CString(sql)
	defer C.free(unsafe.Pointer(cs))
	var errmsg *C.char
	if rc := C.sqlite3_exec(c.h, cs, nil, nil, &errmsg); rc != C.SQLITE_OK {
		if errmsg != nil {
			msg := C.GoString(errmsg)
			C.sqlite3_free(unsafe.Pointer(errmsg))
			return errors.New(msg)
		}
		return c.err()
	}
	return nil
}

func (c *sqliteConn) prepare(sql string) (*sqliteStmt, error) {
	cs := C.CString(sql)
	defer C.free(unsafe.Pointer(cs))
	var st *C.sqlite3_stmt
	if rc := C.sqlite3_prepare_v2(c.h, cs, -1, &st, nil); rc != C.SQLITE_OK {
		return nil, c.err()
	}
	if st == nil {
		return nil, errors.New("sqlite: empty statement")
	}
	return &sqliteStmt{c: c, s: st}, nil
}

func (s *sqliteStmt) reset() { C.sqlite3_reset(s.s) }
func (s *sqliteStmt) close() { C.sqlite3_finalize(s.s) }

func (s *sqliteStmt) bindInt64(i int, v int64) error {
	if rc := C.sqlite3_bind_int64(s.s, C.int(i), C.sqlite3_int64(v)); rc != C.SQLITE_OK {
		return s.c.err()
	}
	return nil
}

func (s *sqliteStmt) bindText(i int, v string) error {
	p := (*C.char)(unsafe.Pointer(&emptyByte))
	if len(v) > 0 {
		p = (*C.char)(unsafe.Pointer(unsafe.StringData(v)))
	}
	if rc := C.bind_text_transient(s.s, C.int(i), p, C.int(len(v))); rc != C.SQLITE_OK {
		return s.c.err()
	}
	return nil
}

func (s *sqliteStmt) step() (bool, error) {
	switch rc := C.sqlite3_step(s.s); rc {
	case C.SQLITE_ROW:
		return true, nil
	case C.SQLITE_DONE:
		return false, nil
	default:
		return false, s.c.err()
	}
}

func (s *sqliteStmt) columnInt64(i int) int64 {
	return int64(C.sqlite3_column_int64(s.s, C.int(i)))
}

// columnText points into SQLite's statement buffer. It is only valid until the
// next step/reset; callers append it into the response buffer immediately.
func (s *sqliteStmt) columnText(i int) []byte {
	p := C.sqlite3_column_text(s.s, C.int(i))
	if p == nil {
		return nil
	}
	n := C.sqlite3_column_bytes(s.s, C.int(i))
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n))
}

func (s *sqliteStmt) changes() int {
	return int(C.sqlite3_changes(s.c.h))
}

const (
	feedSQL = `
		SELECT json_group_array(json_object(
			'id', p.id,
			'body', p.body,
			'created_at', p.created_at,
			'author', p.username,
			'like_count', p.like_count))
		FROM (
			SELECT p.id, p.body, p.created_at, u.username,
			       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
			  FROM posts p JOIN users u ON u.id = p.user_id
			 ORDER BY p.created_at DESC, p.id DESC
			 LIMIT 20
		) p`
	getPostSQL = `
		SELECT json_object('post', json_object(
			'id', p.id,
			'body', p.body,
			'created_at', p.created_at,
			'author', u.username,
			'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)))
		  FROM posts p JOIN users u ON u.id = p.user_id
		 WHERE p.id = ?`
	insertPostSQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
	postExistsSQL = "SELECT 1 FROM posts WHERE id = ?"
	insertLikeSQL = `INSERT INTO likes (user_id, post_id)
		SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
		ON CONFLICT (user_id, post_id) DO NOTHING`
	healthSQL = "SELECT 1"
)

type readConn struct {
	db        *sqliteConn
	feed      *sqliteStmt
	getPost   *sqliteStmt
	health    *sqliteStmt
	postStmts map[int]*sqliteStmt
}

func newReadConn(path string) (*readConn, error) {
	db, err := openSQLite(path)
	if err != nil {
		return nil, err
	}
	rc := &readConn{db: db, postStmts: make(map[int]*sqliteStmt)}
	if rc.feed, err = db.prepare(feedSQL); err != nil {
		db.close()
		return nil, err
	}
	if rc.getPost, err = db.prepare(getPostSQL); err != nil {
		db.close()
		return nil, err
	}
	if rc.health, err = db.prepare(healthSQL); err != nil {
		db.close()
		return nil, err
	}
	return rc, nil
}

type writeConn struct {
	db         *sqliteConn
	insertPost *sqliteStmt
	postExists *sqliteStmt
	insertLike *sqliteStmt
}

func newWriteConn(path string) (*writeConn, error) {
	db, err := openSQLite(path)
	if err != nil {
		return nil, err
	}
	wc := &writeConn{db: db}
	if wc.insertPost, err = db.prepare(insertPostSQL); err != nil {
		db.close()
		return nil, err
	}
	if wc.postExists, err = db.prepare(postExistsSQL); err != nil {
		db.close()
		return nil, err
	}
	if wc.insertLike, err = db.prepare(insertLikeSQL); err != nil {
		db.close()
		return nil, err
	}
	return wc, nil
}
