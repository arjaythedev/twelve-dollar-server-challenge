package main

import "core:c"
import "core:fmt"
import "core:os"
import "core:strings"
import "core:time"

db: Sqlite3

// Prepared once, reused for every request (they cache the plan, not the data).
stmt_feed:       Sqlite3_Stmt
stmt_feed_ids:   Sqlite3_Stmt
stmt_feed_likes: Sqlite3_Stmt
stmt_feed_rows:  Sqlite3_Stmt
stmt_post:       Sqlite3_Stmt
stmt_create:     Sqlite3_Stmt
stmt_exists:     Sqlite3_Stmt
stmt_like:       Sqlite3_Stmt
stmt_health:     Sqlite3_Stmt

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

	stmt_feed       = db_prepare(POST_COLUMNS + `ORDER BY p.created_at DESC, p.id DESC LIMIT 20`)
	stmt_feed_ids   = db_prepare(`SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20`)
	stmt_feed_likes = db_prepare(`SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id ORDER BY post_id DESC`)
	stmt_feed_rows  = db_prepare(`SELECT p.id, p.body, p.created_at, u.username FROM posts p CROSS JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC`)
	stmt_post       = db_prepare(POST_COLUMNS + `WHERE p.id = ?1`)
	stmt_create     = db_prepare(`INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at`)
	stmt_exists     = db_prepare(`SELECT 1 FROM posts WHERE id = ?1`)
	stmt_like       = db_prepare(`INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING`)
	stmt_health     = db_prepare(`SELECT 1`)
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

	// The range-scan feed must produce exactly the reference query's bytes on this database.
	out_reset()
	if !write_feed_reference() { db_fatal("reference feed") }
	reference := strings.clone(string(out_bytes()))
	defer delete(reference)
	out_reset()
	if !write_feed() || string(out_bytes()) != reference { db_fatal("range-scan feed differs from the reference query") }

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

FEED_SIZE :: 20

// {"posts":[...20 newest...]}. The newest posts almost always have consecutive ids, so instead of
// the reference query's 60 B-tree descents (row, author and like count for each post), read the 20
// ids from the feed index, then the rows and the like counts as two scans over that id range.
// When the ids are not consecutive, run the reference query.
write_feed :: proc() -> bool {
	ids: [FEED_SIZE]i64
	n := 0
	rc: c.int
	for rc = step(stmt_feed_ids); rc == SQLITE_ROW; rc = step(stmt_feed_ids) {
		ids[n] = column_int64(stmt_feed_ids, 0)
		n += 1
	}
	reset(stmt_feed_ids)
	if rc != SQLITE_DONE { return false }
	if n == 0 { return write_feed_reference() }
	for i in 1 ..< n {
		if ids[i] != ids[0] - i64(i) { return write_feed_reference() }
	}

	// Consecutive ids in feed order: the range holds exactly these posts, and id order is feed order.
	defer reset(stmt_feed_rows)
	defer reset(stmt_feed_likes)
	bind_int64(stmt_feed_rows, 1, ids[n - 1])
	bind_int64(stmt_feed_rows, 2, ids[0])
	bind_int64(stmt_feed_likes, 1, ids[n - 1])
	bind_int64(stmt_feed_likes, 2, ids[0])
	like_rc := step(stmt_feed_likes)
	write_str(`{"posts":[`)
	first := true
	for rc = step(stmt_feed_rows); rc == SQLITE_ROW; rc = step(stmt_feed_rows) {
		id := column_int64(stmt_feed_rows, 0)
		for like_rc == SQLITE_ROW && column_int64(stmt_feed_likes, 0) > id { like_rc = step(stmt_feed_likes) }
		likes: i64
		if like_rc == SQLITE_ROW && column_int64(stmt_feed_likes, 0) == id { likes = column_int64(stmt_feed_likes, 1) }
		if !first { write_str(",") }
		first = false
		write_post(id, column_string(stmt_feed_rows, 1), column_string(stmt_feed_rows, 2), column_string(stmt_feed_rows, 3), likes)
	}
	write_str(`]}`)
	return rc == SQLITE_DONE && (like_rc == SQLITE_ROW || like_rc == SQLITE_DONE)
}

// The feed as the spec's reference query.
write_feed_reference :: proc() -> bool {
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
