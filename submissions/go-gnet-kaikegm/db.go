package main

/*
#cgo CFLAGS: -O3 -DNDEBUG -fomit-frame-pointer -finline-functions -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0 -DSQLITE_ENABLE_MATH_FUNCTIONS=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_USE_ALLOCA=1 -DSQLITE_DIRECT_OVERFLOW_READ=1 -DSQLITE_OMIT_PROGRESS_CALLBACK=1 -DSQLITE_OMIT_SHARED_CACHE=1 -DSQLITE_OMIT_DECLTYPE=1 -DSQLITE_OMIT_EXPLAIN=1 -DSQLITE_OMIT_AUTHORIZATION=1 -DSQLITE_MAX_MMAP_SIZE=2147483648 -DHAVE_USLEEP=1
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include <string.h>
#include "sqlite/sqlite3.h"

static inline char* append_uint(char *p, sqlite3_int64 v) {
	if (v <= 0) {
		*p++ = '0';
		return p;
	}
	char tmp[24];
	int i = 0;
	while (v > 0) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0) {
		*p++ = tmp[--i];
	}
	return p;
}

static inline char* append_json_str(char *p, const unsigned char *s, int len) {
	*p++ = '"';
	int start = 0;
	for (int i = 0; i < len; i++) {
		unsigned char c = s[i];
		if (c < 0x20 || c == '"' || c == '\\') {
			if (i > start) {
				int span = i - start;
				memcpy(p, s + start, span);
				p += span;
			}
			*p++ = '\\';
			switch (c) {
			case '"':
			case '\\':
				*p++ = (char)c;
				break;
			case '\n':
				*p++ = 'n';
				break;
			case '\r':
				*p++ = 'r';
				break;
			case '\t':
				*p++ = 't';
				break;
			case '\b':
				*p++ = 'b';
				break;
			case '\f':
				*p++ = 'f';
				break;
			default: {
				static const char hex[] = "0123456789abcdef";
				*p++ = 'u';
				*p++ = '0';
				*p++ = '0';
				*p++ = hex[c >> 4];
				*p++ = hex[c & 15];
				break;
			}
			}
			start = i + 1;
		}
	}
	if (len > start) {
		int span = len - start;
		memcpy(p, s + start, span);
		p += span;
	}
	*p++ = '"';
	return p;
}

static inline char* append_post_row(char *p, sqlite3_stmt *s) {
	memcpy(p, "{\"id\":", 6);
	p += 6;
	p = append_uint(p, sqlite3_column_int64(s, 0));
	memcpy(p, ",\"body\":", 8);
	p += 8;
	p = append_json_str(p, sqlite3_column_text(s, 1), sqlite3_column_bytes(s, 1));
	memcpy(p, ",\"created_at\":\"", 15);
	p += 15;
	int clen = sqlite3_column_bytes(s, 2);
	memcpy(p, sqlite3_column_text(s, 2), clen);
	p += clen;
	memcpy(p, "\",\"author\":", 11);
	p += 11;
	p = append_json_str(p, sqlite3_column_text(s, 3), sqlite3_column_bytes(s, 3));
	memcpy(p, ",\"like_count\":", 14);
	p += 14;
	p = append_uint(p, sqlite3_column_int64(s, 4));
	*p++ = '}';
	return p;
}

// q_feed: iterates up to 20 rows and serializes {"posts":[...]} directly into buf in a single CGO call.
static int q_feed(sqlite3_stmt *s, char *buf, int cap, int *n) {
	char *p = buf;
	memcpy(p, "{\"posts\":[", 10);
	p += 10;
	int first = 1;
	int rc;
	while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
		if ((p - buf) + 4096 > cap) {
			sqlite3_reset(s);
			*n = cap * 2;
			return SQLITE_ROW;
		}
		if (!first) {
			*p++ = ',';
		}
		first = 0;
		p = append_post_row(p, s);
	}
	*p++ = ']';
	*p++ = '}';
	*n = (int)(p - buf);
	sqlite3_reset(s);
	return rc;
}

// q_post: fetches a single post by ID and serializes {"post":{...}} directly into buf in a single CGO call.
static int q_post(sqlite3_stmt *s, sqlite3_int64 id, char *buf, int cap, int *n) {
	sqlite3_bind_int64(s, 1, id);
	int rc = sqlite3_step(s);
	if (rc == SQLITE_ROW) {
		if (cap < 4096) {
			sqlite3_reset(s);
			*n = 4096;
			return SQLITE_ROW;
		}
		char *p = buf;
		memcpy(p, "{\"post\":", 8);
		p += 8;
		p = append_post_row(p, s);
		*p++ = '}';
		*n = (int)(p - buf);
	}
	sqlite3_reset(s);
	return rc;
}

// q_ping: steps SELECT 1 and resets with zero buffer copies.
static int q_ping(sqlite3_stmt *s) {
	int rc = sqlite3_step(s);
	sqlite3_reset(s);
	return rc;
}

// q_insert: INSERT ... RETURNING id, created_at. Steps to SQLITE_DONE so the row is committed.
static int q_insert(sqlite3_stmt *s, sqlite3_int64 uid, const char *body, int blen,
                    sqlite3_int64 *id, char *created, int cap, int *n) {
	sqlite3_bind_int64(s, 1, uid);
	sqlite3_bind_text(s, 2, body, blen, SQLITE_TRANSIENT);
	int rc = sqlite3_step(s);
	if (rc == SQLITE_ROW) {
		*id = sqlite3_column_int64(s, 0);
		const unsigned char *t = sqlite3_column_text(s, 1);
		int len = sqlite3_column_bytes(s, 1);
		if (len > cap) len = cap;
		memcpy(created, t, len);
		*n = len;
		rc = sqlite3_step(s);
	}
	sqlite3_reset(s);
	return rc;
}

// q_like: binds (uid, pid, pid), checks changes; if 0, steps s_exists in C.
static int q_like(sqlite3_stmt *s_like, sqlite3_stmt *s_exists, sqlite3_int64 uid, sqlite3_int64 pid, int *inserted, int *exists) {
	sqlite3_bind_int64(s_like, 1, uid);
	sqlite3_bind_int64(s_like, 2, pid);
	sqlite3_bind_int64(s_like, 3, pid);
	int rc = sqlite3_step(s_like);
	int ch = sqlite3_changes(sqlite3_db_handle(s_like));
	sqlite3_reset(s_like);
	if (rc != SQLITE_DONE) return rc;
	if (ch == 1) {
		*inserted = 1;
		*exists = 1;
		return SQLITE_DONE;
	}
	*inserted = 0;
	sqlite3_bind_int64(s_exists, 1, pid);
	int rc2 = sqlite3_step(s_exists);
	*exists = (rc2 == SQLITE_ROW) ? 1 : 0;
	sqlite3_reset(s_exists);
	return (rc2 == SQLITE_ROW || rc2 == SQLITE_DONE) ? SQLITE_DONE : rc2;
}

static int prep(sqlite3 *db, const char *sql, sqlite3_stmt **s) {
	return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, 0);
}
*/
import "C"

import (
	"errors"
	"log"
	"slices"
	"unsafe"
)

const (
	feedSQL   = `SELECT p.id, p.body, p.created_at, u.username, (SELECT count(1) FROM likes WHERE post_id = p.id) FROM posts p CROSS JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20`
	postSQL   = `SELECT p.id, p.body, p.created_at, u.username, (SELECT count(1) FROM likes WHERE post_id = p.id) FROM posts p CROSS JOIN users u ON u.id = p.user_id WHERE p.id = ?`
	existsSQL = "SELECT 1 FROM posts WHERE id = ?"
	insertSQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
	likeSQL   = "INSERT INTO likes (user_id, post_id) SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?) ON CONFLICT DO NOTHING"
	pragmas   = "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=10000; PRAGMA journal_size_limit=67108864; PRAGMA busy_timeout=5000; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16384; PRAGMA temp_store=MEMORY; PRAGMA automatic_index=0; PRAGMA threads=0; PRAGMA cell_size_check=0; PRAGMA foreign_keys=OFF; PRAGMA secure_delete=0;"
)

var (
	// Main database handle: accessed exclusively by the single-threaded event loop (0 locks, 0 mutex contention!)
	db                                           *C.sqlite3
	feedS, postS, existsS, insertS, likeS, pingS *C.sqlite3_stmt
	createdScratch                               [64]byte
)

func dbErr() error { return errors.New(C.GoString(C.sqlite3_errmsg(db))) }

func dbOpen(path string) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	if C.sqlite3_open_v2(cpath, &db, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_NOMUTEX, nil) != C.SQLITE_OK {
		log.Fatal("open: ", dbErr())
	}
	cp := C.CString(pragmas)
	defer C.free(unsafe.Pointer(cp))
	if C.sqlite3_exec(db, cp, nil, nil, nil) != C.SQLITE_OK {
		log.Fatal("pragmas: ", dbErr())
	}

	for _, p := range []struct {
		s   **C.sqlite3_stmt
		sql string
	}{
		{&feedS, feedSQL},
		{&postS, postSQL},
		{&existsS, existsSQL},
		{&insertS, insertSQL},
		{&likeS, likeSQL},
		{&pingS, "SELECT 1"},
	} {
		cs := C.CString(p.sql)
		rc := C.prep(db, cs, p.s)
		C.free(unsafe.Pointer(cs))
		if rc != C.SQLITE_OK {
			log.Fatal("prepare: ", dbErr())
		}
	}
}

func dbFeed(out []byte) ([]byte, error) {
	for {
		if cap(out)-len(out) < 64<<10 {
			out = slices.Grow(out, 64<<10)
		}
		spare := out[len(out):cap(out)]
		var n C.int
		rc := C.q_feed(feedS, (*C.char)(unsafe.Pointer(&spare[0])), C.int(len(spare)), &n)
		if rc == C.SQLITE_ROW && int(n) > len(spare) {
			out = slices.Grow(out, int(n))
			continue
		}
		if rc != C.SQLITE_DONE {
			return out, dbErr()
		}
		return out[:len(out)+int(n)], nil
	}
}

func dbPost(out []byte, id int64) ([]byte, bool, error) {
	if cap(out)-len(out) < 4096 {
		out = slices.Grow(out, 4096)
	}
	spare := out[len(out):cap(out)]
	var n C.int
	rc := C.q_post(postS, C.sqlite3_int64(id), (*C.char)(unsafe.Pointer(&spare[0])), C.int(len(spare)), &n)
	switch rc {
	case C.SQLITE_DONE:
		return out, false, nil
	case C.SQLITE_ROW:
		return out[:len(out)+int(n)], true, nil
	default:
		return out, false, dbErr()
	}
}

func dbPing() error {
	rc := C.q_ping(pingS)
	if rc != C.SQLITE_ROW && rc != C.SQLITE_DONE {
		return dbErr()
	}
	return nil
}

func dbInsert(uid int64, body string) (int64, []byte, error) {
	var id C.sqlite3_int64
	var n C.int
	rc := C.q_insert(insertS, C.sqlite3_int64(uid), (*C.char)(unsafe.Pointer(unsafe.StringData(body))), C.int(len(body)),
		&id, (*C.char)(unsafe.Pointer(&createdScratch[0])), C.int(len(createdScratch)), &n)
	if rc != C.SQLITE_DONE {
		return 0, nil, dbErr()
	}
	return int64(id), createdScratch[:n], nil
}

func dbLike(uid, id int64) (inserted, exists bool, err error) {
	var ins, ex C.int
	rc := C.q_like(likeS, existsS, C.sqlite3_int64(uid), C.sqlite3_int64(id), &ins, &ex)
	if rc != C.SQLITE_DONE {
		return false, false, dbErr()
	}
	return ins == 1, ex == 1, nil
}
