use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr;

#[link(name = "btree", kind = "static")]
extern "C" {
    fn sqlite3_open(filename: *const c_char, db: *mut *mut c_void) -> c_int;
    fn sqlite3_close(db: *mut c_void) -> c_int;
    fn sqlite3_exec(db: *mut c_void, sql: *const c_char, callback: Option<unsafe extern "C" fn(*mut c_void, c_int, *mut *mut c_char, *mut *mut c_char) -> c_int>, arg: *mut c_void, err: *mut *mut c_char) -> c_int;
    fn sqlite3_prepare_v2(db: *mut c_void, sql: *const c_char, n: c_int, stmt: *mut *mut c_void, tail: *mut *const c_char) -> c_int;
    fn sqlite3_step(stmt: *mut c_void) -> c_int;
    fn sqlite3_column_int(stmt: *mut c_void, col: c_int) -> c_int;
    fn sqlite3_finalize(stmt: *mut c_void) -> c_int;
    fn btree_reader_new(db: *mut c_void, posts: u32, users: u32, feed: u32, likes: u32) -> *mut c_void;
    fn btree_reader_begin(reader: *mut c_void) -> c_int;
    fn btree_reader_reopen(reader: *mut c_void) -> c_int;
    fn btree_reader_close_cursors(reader: *mut c_void);
    fn btree_reader_end(reader: *mut c_void, ok: c_int) -> c_int;
    fn btree_reader_refresh_feed(reader: *mut c_void);
    fn btree_reader_refresh_post(reader: *mut c_void);
    fn btree_reader_refresh_like(reader: *mut c_void);
    fn btree_reader_free(reader: *mut c_void);
    fn feed_btree_tx(reader: *mut c_void, out: *mut c_char, cap: c_int, len: *mut c_int) -> c_int;
}

#[test]
fn feed_counts_likes_across_extreme_signed_post_ids() {
    unsafe {
        let mut db = ptr::null_mut();
        assert_eq!(sqlite3_open(c":memory:".as_ptr(), &mut db), 0);
        let sql = CString::new(r#"
            CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL);
            CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, body TEXT NOT NULL, created_at TEXT NOT NULL);
            CREATE TABLE likes (user_id INTEGER NOT NULL, post_id INTEGER NOT NULL, PRIMARY KEY(user_id, post_id));
            CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC, id DESC);
            CREATE INDEX likes_post_id_idx ON likes(post_id);
            INSERT INTO users VALUES(1, 'first'), (2, 'second');
            INSERT INTO posts VALUES
              (-9223372036854775808, 1, 'minimum', '2030-01-01T00:00:00.000Z'),
              (-140737488355328, 2, 'negative six-byte', '2030-01-01T00:00:01.000Z'),
              (1, 1, 'middle', '2030-01-01T00:00:02.000Z'),
              (2147483648, 2, 'positive six-byte', '2030-01-01T00:00:03.000Z'),
              (9223372036854775807, 2, 'maximum', '2030-01-01T00:00:04.000Z');
            INSERT INTO likes VALUES(1, -9223372036854775808), (2, -9223372036854775808),
                                    (1, -140737488355328), (1, 1), (1, 2147483648),
                                    (1, 9223372036854775807);
        "#).unwrap();
        assert_eq!(sqlite3_exec(db, sql.as_ptr(), None, ptr::null_mut(), ptr::null_mut()), 0);
        let mut roots = [0u32; 4];
        for (i, name) in ["posts", "users", "posts_created_at_id_idx", "likes_post_id_idx"].iter().enumerate() {
            let sql = CString::new(format!("SELECT rootpage FROM sqlite_master WHERE name='{name}'")).unwrap();
            let mut stmt = ptr::null_mut();
            assert_eq!(sqlite3_prepare_v2(db, sql.as_ptr(), -1, &mut stmt, ptr::null_mut()), 0);
            assert_eq!(sqlite3_step(stmt), 100);
            roots[i] = sqlite3_column_int(stmt, 0) as u32;
            assert_eq!(sqlite3_finalize(stmt), 0);
        }
        let reader = btree_reader_new(db, roots[0], roots[1], roots[2], roots[3]);
        assert!(!reader.is_null());
        assert_eq!(btree_reader_begin(reader), 0);
        assert_eq!(btree_reader_reopen(reader), 0);
        let mut bytes = vec![0u8; 65536];
        let mut len = 0;
        assert_eq!(feed_btree_tx(reader, bytes.as_mut_ptr().cast(), bytes.len() as c_int, &mut len), 0);
        let actual: serde_json::Value = serde_json::from_slice(&bytes[..len as usize]).unwrap();
        assert_eq!(actual, serde_json::json!([
            {"id": i64::MAX, "body": "maximum", "created_at": "2030-01-01T00:00:04.000Z", "author": "second", "like_count": 1},
            {"id": 2147483648i64, "body": "positive six-byte", "created_at": "2030-01-01T00:00:03.000Z", "author": "second", "like_count": 1},
            {"id": 1, "body": "middle", "created_at": "2030-01-01T00:00:02.000Z", "author": "first", "like_count": 1},
            {"id": -140737488355328i64, "body": "negative six-byte", "created_at": "2030-01-01T00:00:01.000Z", "author": "second", "like_count": 1},
            {"id": i64::MIN, "body": "minimum", "created_at": "2030-01-01T00:00:00.000Z", "author": "first", "like_count": 2}
        ]));
        btree_reader_close_cursors(reader);
        assert_eq!(btree_reader_end(reader, 1), 0);
        let update = CString::new("INSERT INTO likes VALUES(2, 1); INSERT INTO posts VALUES(2, 2, 'new', '2030-01-01T00:00:05.000Z');").unwrap();
        assert_eq!(sqlite3_exec(db, update.as_ptr(), None, ptr::null_mut(), ptr::null_mut()), 0);
        btree_reader_refresh_feed(reader);
        btree_reader_refresh_post(reader);
        btree_reader_refresh_like(reader);
        assert_eq!(btree_reader_begin(reader), 0);
        assert_eq!(btree_reader_reopen(reader), 0);
        assert_eq!(feed_btree_tx(reader, bytes.as_mut_ptr().cast(), bytes.len() as c_int, &mut len), 0);
        let refreshed: serde_json::Value = serde_json::from_slice(&bytes[..len as usize]).unwrap();
        assert_eq!(refreshed[0]["id"], 2);
        let middle = refreshed.as_array().unwrap().iter().find(|post| post["id"] == 1).unwrap();
        assert_eq!(middle["like_count"], 2);
        btree_reader_close_cursors(reader);
        assert_eq!(btree_reader_end(reader, 1), 0);
        btree_reader_free(reader);
        assert_eq!(sqlite3_close(db), 0);
    }
}
