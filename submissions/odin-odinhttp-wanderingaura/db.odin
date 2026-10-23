package main

import "core:c"
import "core:fmt"
import "core:os"
import "core:strings"
import "core:time"

db: Sqlite3

// Prepared once, reused for every request (they cache the plan, not the data).
stmt_feed:   Sqlite3_Stmt
stmt_post:   Sqlite3_Stmt
stmt_create: Sqlite3_Stmt
stmt_exists: Sqlite3_Stmt
stmt_like:   Sqlite3_Stmt
stmt_health: Sqlite3_Stmt

POST_COLUMNS :: `SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id `

db_fatal :: proc(what: string) -> ! {
	fmt.eprintfln("sqlite: %s: %s", what, errmsg(db) if db != nil else "")
	os.exit(1)
}

db_exec :: proc(sql: cstring) {
	if exec(db, sql, nil, nil, nil) != SQLITE_OK { db_fatal(string(sql)) }
}

db_prepare :: proc(sql: cstring) -> (stmt: Sqlite3_Stmt) {
	if prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, nil) != SQLITE_OK { db_fatal(string(sql)) }
	return
}

// Runs a one-row query and returns its first column as text.
db_query_text :: proc(sql: cstring) -> string {
	stmt := db_prepare(sql)
	defer finalize(stmt)
	if step(stmt) != SQLITE_ROW { db_fatal(string(sql)) }
	return strings.clone(string(column_string(stmt, 0)))
}

db_open :: proc(path: string) {
	// SQLite's page cache comes from a pre-touched slab instead of malloc.
	hdr: c.int
	config(SQLITE_CONFIG_PCACHE_HDRSZ, &hdr)
	slot := c.int(SQLITE_PAGE_SIZE) + hdr
	cache := slab_alloc(int(slot) * SQLITE_CACHE_PAGES)
	config(SQLITE_CONFIG_PAGECACHE, raw_data(cache), slot, c.int(SQLITE_CACHE_PAGES))
	if initialize() != SQLITE_OK { db_fatal("initialize") }

	if open_v2(strings.clone_to_cstring(path), &db, SQLITE_OPEN_READWRITE, nil) != SQLITE_OK { db_fatal("open") }

	// EXCLUSIVE before the WAL is first touched: the wal-index then lives in heap memory, so
	// there is no -shm file and no fcntl locking on every transaction.
	db_exec("PRAGMA locking_mode = EXCLUSIVE")
	if mode := db_query_text("PRAGMA journal_mode = WAL"); mode != "wal" {
		fmt.eprintfln("sqlite: journal_mode is %q, want wal", mode)
		os.exit(1)
	}
	db_exec("PRAGMA synchronous = NORMAL")
	db_exec("PRAGMA mmap_size = 1073741824")
	db_exec(fmt.ctprintf("PRAGMA cache_size = %d", SQLITE_CACHE_PAGES))
	db_exec("PRAGMA temp_store = MEMORY")

	stmt_feed   = db_prepare(POST_COLUMNS + `ORDER BY p.created_at DESC, p.id DESC LIMIT 20`)
	stmt_post   = db_prepare(POST_COLUMNS + `WHERE p.id = ?1`)
	stmt_create = db_prepare(`INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at`)
	stmt_exists = db_prepare(`SELECT 1 FROM posts WHERE id = ?1`)
	stmt_like   = db_prepare(`INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING`)
	stmt_health = db_prepare(`SELECT 1`)
}

// Faults in every page the queries touch through SQLite's own mmap, so the page tables exist
// before the first request, then runs the read statements a few times.
db_warm_up :: proc() {
	start := time.tick_now()
	warm := [?]cstring{
		`SELECT sum(length(body)) FROM posts`,
		`SELECT sum(length(username)) FROM users`,
		`SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx`,
		`SELECT count(*) FROM likes INDEXED BY likes_post_id_idx`,
		`SELECT count(*) FROM likes INDEXED BY sqlite_autoindex_likes_1`,
	}
	for sql in warm {
		stmt := db_prepare(sql)
		if step(stmt) != SQLITE_ROW { db_fatal(string(sql)) }
		finalize(stmt)
	}
	for _ in 0 ..< 100 {
		out_reset()
		write_feed()
		for id in ([]i64{1, 250000, 500000}) {
			out_reset()
			write_post_by_id(id)
		}
	}
	out_reset()
	fmt.printfln("warm-up took %v", time.tick_since(start))
}

// {"posts":[...20 newest...]}
write_feed :: proc() -> bool {
	defer reset(stmt_feed)
	write_str(`{"posts":[`)
	first := true
	rc: c.int
	for rc = step(stmt_feed); rc == SQLITE_ROW; rc = step(stmt_feed) {
		if !first { write_str(",") }
		first = false
		write_post_columns(stmt_feed)
	}
	write_str(`]}`)
	return rc == SQLITE_DONE
}

Lookup :: enum { Found, Missing, Failed }

// {"post":{...}}
write_post_by_id :: proc(id: i64) -> Lookup {
	defer reset(stmt_post)
	bind_int64(stmt_post, 1, id)
	switch step(stmt_post) {
	case SQLITE_ROW:
		write_str(`{"post":`)
		write_post_columns(stmt_post)
		write_str(`}`)
		return .Found
	case SQLITE_DONE:
		return .Missing
	}
	return .Failed
}

write_post_columns :: #force_inline proc(stmt: Sqlite3_Stmt) {
	write_post(
		column_int64(stmt, 0),
		column_string(stmt, 1),
		column_string(stmt, 2),
		column_string(stmt, 3),
		column_int64(stmt, 4),
	)
}

post_exists :: proc(id: i64) -> Lookup {
	defer reset(stmt_exists)
	bind_int64(stmt_exists, 1, id)
	switch step(stmt_exists) {
	case SQLITE_ROW:  return .Found
	case SQLITE_DONE: return .Missing
	}
	return .Failed
}

// Inserts the like; true in `inserted` when it is new. Committed before this returns.
insert_like :: proc(user_id, post_id: i64) -> (inserted: bool, ok: bool) {
	defer reset(stmt_like)
	bind_int64(stmt_like, 1, user_id)
	bind_int64(stmt_like, 2, post_id)
	if step(stmt_like) != SQLITE_DONE { return false, false }
	return changes(db) == 1, true
}

// Inserts the post and writes {"post":{...}}. Committed before this returns.
create_post :: proc(user_id: i64, body: string, author: string) -> bool {
	defer reset(stmt_create)
	bind_int64(stmt_create, 1, user_id)
	bind_text(stmt_create, 2, raw_data(body), c.int(len(body)), SQLITE_TRANSIENT)
	if step(stmt_create) != SQLITE_ROW { return false }
	id := column_int64(stmt_create, 0)
	created_at := column_string(stmt_create, 1)
	start := out.len
	write_str(`{"post":`)
	write_post(id, transmute([]byte)body, created_at, transmute([]byte)author, 0)
	write_str(`}`)
	// The autocommit transaction commits when the statement runs to completion.
	if step(stmt_create) != SQLITE_DONE {
		out.len = start
		return false
	}
	return true
}

db_healthy :: proc() -> bool {
	defer reset(stmt_health)
	return step(stmt_health) == SQLITE_ROW
}
