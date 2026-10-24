module main

import os

// SQL text is identical to the reference queries in SPEC.md; only the like insert is guarded
// by EXISTS so a missing post and an existing like can be told apart with one statement.
const sql_select_post = 'SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id'
const sql_feed = sql_select_post + ' ORDER BY p.created_at DESC,p.id DESC LIMIT 20'
const sql_post = sql_select_post + ' WHERE p.id=?1'
const sql_health = 'SELECT 1'
const sql_insert = 'INSERT INTO posts(user_id,body) VALUES(?1,?2)'
const sql_created = 'SELECT created_at FROM posts WHERE id=?1'
const sql_like = 'INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING'
const sql_exists = 'SELECT 1 FROM posts WHERE id=?1'
// The feed's range path: the newest 20 ids, then two range scans instead of 20 lookups and
// 20 correlated counts. Used only when the 20 ids are close together (see feed_by_range).
const sql_feed_ids = 'SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20'
const sql_feed_likes = 'SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2'
const sql_feed_rows = 'SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2'

// ckpt_frames is the WAL size (in frames) that triggers a checkpoint after the next commit.
const ckpt_frames = 4096

// Db owns the single SQLite connection of an event loop and its prepared statements.
struct Db {
mut:
	conn       &C.sqlite3      = unsafe { nil }
	st_feed    &C.sqlite3_stmt = unsafe { nil }
	st_post    &C.sqlite3_stmt = unsafe { nil }
	st_health  &C.sqlite3_stmt = unsafe { nil }
	st_insert  &C.sqlite3_stmt = unsafe { nil }
	st_created &C.sqlite3_stmt = unsafe { nil }
	st_like    &C.sqlite3_stmt = unsafe { nil }
	st_exists  &C.sqlite3_stmt = unsafe { nil }
	st_ids     &C.sqlite3_stmt = unsafe { nil }
	st_likes   &C.sqlite3_stmt = unsafe { nil }
	st_rows    &C.sqlite3_stmt = unsafe { nil }
	st_begin   &C.sqlite3_stmt = unsafe { nil }
	st_commit  &C.sqlite3_stmt = unsafe { nil }
	st_back    &C.sqlite3_stmt = unsafe { nil }
	// group makes every statement run inside one deferred transaction that is committed by
	// commit() (once per event batch). With group off each statement autocommits.
	group      bool
	in_txn     bool
	aborted    bool // SQLite rolled the batch transaction back by itself (see begin)
	wal_frames int  // frames in the WAL after the last commit, from the wal hook
	exclusive  bool
}

// db_pragmas configures the connection the way the reference Rust entry does.
fn db_pragmas(exclusive bool) string {
	mut s := ''
	if exclusive {
		s += 'PRAGMA locking_mode=EXCLUSIVE;'
	} else {
		// several connections (fasthttp workers, or several processes) share the file
		s += 'PRAGMA busy_timeout=5000;'
	}
	s += 'PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;'
	if os.getenv('DB_MMAP') == '0' {
		// page cache instead of mmap: 64 MiB of pages owned by SQLite
		s += 'PRAGMA mmap_size=0;PRAGMA cache_size=-65536;PRAGMA temp_store=MEMORY;'
	} else {
		s += 'PRAGMA mmap_size=1073741824;PRAGMA cache_size=500;PRAGMA temp_store=MEMORY;'
	}
	s += 'PRAGMA journal_size_limit=67108864;PRAGMA wal_autocheckpoint=0;'
	s += 'SELECT count(*) FROM sqlite_schema;'
	return s
}

fn wal_hook(arg voidptr, _ &C.sqlite3, _ &char, frames int) int {
	mut d := unsafe { &Db(arg) }
	d.wal_frames = frames
	return sqlite_ok
}

// open_db opens the database at path and prepares every statement once.
fn open_db(path string, group bool, exclusive bool) !&Db {
	mut d := &Db{
		group:     group
		exclusive: exclusive
	}
	if C.sqlite3_open_v2(&char(path.str), &d.conn, sqlite_open_readwrite | sqlite_open_nomutex,
		unsafe { nil }) != sqlite_ok {
		return error('sqlite open ${path}: ${d.errmsg()}')
	}
	pragmas := db_pragmas(exclusive)
	if C.sqlite3_exec(d.conn, &char(pragmas.str), unsafe { nil }, unsafe { nil }, unsafe { nil }) != sqlite_ok {
		return error('sqlite pragmas: ${d.errmsg()}')
	}
	C.sqlite3_wal_hook(d.conn, wal_hook, d)
	d.st_feed = d.prepare(sql_feed)!
	d.st_post = d.prepare(sql_post)!
	d.st_health = d.prepare(sql_health)!
	d.st_insert = d.prepare(sql_insert)!
	d.st_created = d.prepare(sql_created)!
	d.st_like = d.prepare(sql_like)!
	d.st_exists = d.prepare(sql_exists)!
	d.st_ids = d.prepare(sql_feed_ids)!
	d.st_likes = d.prepare(sql_feed_likes)!
	d.st_rows = d.prepare(sql_feed_rows)!
	d.st_begin = d.prepare('BEGIN')!
	d.st_commit = d.prepare('COMMIT')!
	d.st_back = d.prepare('ROLLBACK')!
	return d
}

fn (d &Db) errmsg() string {
	return unsafe { cstring_to_vstring(C.sqlite3_errmsg(d.conn)) }
}

fn (mut d Db) prepare(query string) !&C.sqlite3_stmt {
	mut st := &C.sqlite3_stmt(unsafe { nil })
	if C.sqlite3_prepare_v3(d.conn, &char(query.str), query.len, sqlite_prepare_persistent,
		&st, unsafe { nil }) != sqlite_ok {
		return error('sqlite prepare `${query}`: ${d.errmsg()}')
	}
	return st
}

// run_once steps a statement that returns no rows and resets it.
fn run_once(st &C.sqlite3_stmt) bool {
	rc := C.sqlite3_step(st)
	C.sqlite3_reset(st)
	return rc == sqlite_done
}

// begin opens the batch transaction if grouping is on and none is open.
fn (mut d Db) begin() {
	if !d.group {
		return
	}
	if d.in_txn {
		if C.sqlite3_get_autocommit(d.conn) == 0 {
			return
		}
		// SQLite rolled the batch transaction back on its own (an I/O error, a full disk, out
		// of memory): the earlier statements of this batch are gone, so nothing of the batch
		// may commit. What follows runs in a fresh transaction that commit() rolls back.
		d.aborted = true
	}
	if run_once(d.st_begin) {
		d.in_txn = true
	}
}

// commit ends the batch transaction. It returns false when nothing of the batch was
// committed (COMMIT failed, or SQLite had already rolled the transaction back), so the
// caller can drop every tentative response of the batch.
fn (mut d Db) commit() bool {
	if !d.in_txn {
		return true
	}
	d.in_txn = false
	aborted := d.aborted
	d.aborted = false
	if aborted || C.sqlite3_get_autocommit(d.conn) != 0 {
		run_once(d.st_back)
		return false
	}
	if run_once(d.st_commit) {
		return true
	}
	run_once(d.st_back)
	return false
}

// close finalizes every statement and closes the connection, releasing its locks (a close
// with statements still alive would only leave a zombie that keeps the exclusive lock).
fn (mut d Db) close() {
	for st in [d.st_feed, d.st_post, d.st_health, d.st_insert, d.st_created, d.st_like,
		d.st_exists, d.st_ids, d.st_likes, d.st_rows, d.st_begin, d.st_commit, d.st_back] {
		C.sqlite3_finalize(st)
	}
	rc := C.sqlite3_close(d.conn)
	assert rc == sqlite_ok
	d.conn = unsafe { nil }
}

// checkpoint copies the WAL into the database once it holds ckpt_frames frames.
fn (mut d Db) checkpoint() {
	if d.wal_frames < ckpt_frames || d.in_txn {
		return
	}
	mode := if d.exclusive { sqlite_checkpoint_full } else { sqlite_checkpoint_passive }
	if C.sqlite3_wal_checkpoint_v2(d.conn, unsafe { nil }, mode, unsafe { nil }, unsafe { nil }) == sqlite_ok {
		d.wal_frames = 0
	}
}
