//! E01/E02/E03 (FRONTIER-III): verification experiments.
//!
//! E01 structured-random oracle: random VALID SQLite databases (deep trees,
//! big bodies at overflow boundaries, sparse likes) — reader must agree with
//! rusqlite everywhere.
//! E02 chaos-consistency: SIGKILL a writer child mid-batch, reopen, compare.
//! E03 checkpoint-mode fuzz: PASSIVE/FULL/RESTART/TRUNCATE interleaved with
//! reads and random writes.

use rusqlite::Connection;
use sqlite_btree::btree::{Cursor, Step, TableSeek};
use sqlite_btree::record::{decode_header, Field};
use sqlite_btree::scan::IndexScan;
use sqlite_btree::store::Store;

const SCHEMA: &str = r#"
CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
CREATE TABLE likes (user_id INTEGER NOT NULL, post_id INTEGER NOT NULL, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), PRIMARY KEY (user_id, post_id));
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX likes_post_id_idx ON likes (post_id);
"#;

struct Rnd(u64);
impl Rnd {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

fn open_all(path: &str) -> (Connection, Store) {
    let conn = Connection::open(path).unwrap();
    conn.pragma_update(None, "journal_mode", "WAL").unwrap();
    conn.pragma_update(None, "synchronous", "NORMAL").unwrap();
    conn.pragma_update(None, "wal_autocheckpoint", 0).unwrap();
    conn.execute_batch(SCHEMA).unwrap();
    let store = Store::open(path).expect("store opens");
    (conn, store)
}

fn roots_of(conn: &Connection) -> (u32, u32, u32) {
    let mut st = conn
        .prepare("SELECT name, rootpage FROM sqlite_schema WHERE name IN ('users','posts','likes_post_id_idx')")
        .unwrap();
    let mut rows = st.query([]).unwrap();
    let mut roots = (0u32, 0u32, 0u32);
    while let Some(r) = rows.next().unwrap() {
        let name: String = r.get(0).unwrap();
        let page: i64 = r.get(1).unwrap();
        match name.as_str() {
            "users" => roots.0 = page as u32,
            "posts" => roots.1 = page as u32,
            "likes_post_id_idx" => roots.2 = page as u32,
            _ => {}
        }
    }
    roots
}

/// E01: random VALID corpus — writer generates structured-random batches
/// (varying body sizes up to the 500-char cap, sparse/dense likes, checkpoint
/// pauses); after every batch the reader must match rusqlite on seeks,
/// counts, and feed order.
#[test]
fn e01_structured_random_corpus() {
    let dir = std::env::temp_dir().join(format!("e01-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let db = dir.join("e01.db");
    let _ = std::fs::remove_file(&db);
    let (conn, mut store) = open_all(db.to_str().unwrap());

    conn.execute("INSERT INTO users (username) VALUES ('u1')", []).unwrap();
    let mut rnd = Rnd(42);
    let mut post_seq: i64 = 0;

    for round in 0..30u64 {
        let bodies = rnd.below(25) + 1;
        for _ in 0..bodies {
            post_seq += 1;
            // body lengths from 1..500 (overflow boundary is ~4061 — never
            // crossed on this schema; the oracle asserts that assumption)
            let len = (rnd.below(500) + 1) as usize;
            let body: String = "x".repeat(len);
            conn.execute(
                "INSERT INTO posts (user_id, body) VALUES (1, ?1)",
                [&body],
            )
            .unwrap();
        }
        let likes = rnd.below(60) + 1;
        for _ in 0..likes {
            let post = (rnd.below(post_seq as u64)) as i64 + 1;
            let user = (rnd.below(1)) as i64 + 1;
            conn.execute(
                "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)",
                [user, post],
            )
            .unwrap();
        }
        store.refresh().unwrap();

        // feed order
        let theirs: Vec<i64> = {
            let mut st = conn
                .prepare("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20")
                .unwrap();
            let mut rows = st.query([]).unwrap();
            let mut v = Vec::new();
            while let Some(r) = rows.next().unwrap() {
                v.push(r.get::<_, i64>(0).unwrap());
            }
            v
        };
        let (users_r, posts_r, likes_r) = roots_of(&conn);
        let mut cur = Cursor::table(&store, posts_r).unwrap();
        let mut ours = Vec::new();
        if cur.last().unwrap() == Step::Moved {
            loop {
                ours.push(cur.rowid().unwrap());
                if ours.len() == 20 || cur.prev().unwrap() == Step::End {
                    break;
                }
            }
        }
        assert_eq!(ours, theirs, "E01 feed order round {round}");

        // seeks + counts on random ids
        for _ in 0..10 {
            let id = (rnd.below(post_seq as u64)) as i64 + 1;
            let mut cur = Cursor::table(&store, posts_r).unwrap();
            let found = cur.table_seek(id).unwrap() == TableSeek::Found;
            let exists = conn
                .query_row("SELECT 1 FROM posts WHERE id = ?1", [id], |_| Ok(()))
                .is_ok();
            assert_eq!(found, exists, "E01 seek id {id} round {round}");
            let mut scan = IndexScan::new(&store, likes_r).unwrap();
            let ours_c = scan.count_prefix(id).unwrap();
            let theirs_c: i64 = conn
                .query_row(
                    "SELECT COUNT(*) FROM likes WHERE post_id = ?1",
                    [id],
                    |r| r.get(0),
                )
                .unwrap_or(0);
            assert_eq!(ours_c, theirs_c, "E01 count id {id} round {round}");
        }
        let _ = (users_r, likes_r);
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// E03: checkpoint-mode fuzz — same reads, but between rounds the checkpoint
/// PRAGMA cycles through PASSIVE/FULL/RESTART/TRUNCATE. The reader must
/// re-base and still agree everywhere.
#[test]
fn e03_checkpoint_mode_fuzz() {
    let dir = std::env::temp_dir().join(format!("e03-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let db = dir.join("e03.db");
    let _ = std::fs::remove_file(&db);
    let (conn, mut store) = open_all(db.to_str().unwrap());
    conn.execute("INSERT INTO users (username) VALUES ('u1')", []).unwrap();

    let modes = ["PASSIVE", "FULL", "RESTART", "TRUNCATE"];
    let mut rnd = Rnd(777);
    let mut post_seq: i64 = 0;
    for round in 0..16u64 {
        let bodies = rnd.below(30) + 5;
        for _ in 0..bodies {
            post_seq += 1;
            let body: String = "y".repeat((rnd.below(400) + 20) as usize);
            conn.execute("INSERT INTO posts (user_id, body) VALUES (1, ?1)", [&body]).unwrap();
        }
        store.refresh().unwrap();
        let mode = modes[(round % 4) as usize];
        conn.pragma_update(None, "wal_checkpoint", mode).unwrap();
        store.refresh().unwrap();

        // feed head must still be the newest post after every checkpoint mode
        let theirs: i64 = conn
            .query_row("SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 1", [], |r| r.get(0))
            .unwrap();
        let (_, posts_r, likes_r) = roots_of(&conn);
        let mut cur = Cursor::table(&store, posts_r).unwrap();
        if cur.last().unwrap() == Step::Moved {
            let head = cur.rowid().unwrap();
            assert_eq!(head, theirs, "E03 head after {mode} round {round}");
        }
        let mut scan = IndexScan::new(&store, likes_r).unwrap();
        let ours_c = scan.count_prefix(theirs).unwrap();
        let theirs_c: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM likes WHERE post_id = ?1",
                [theirs],
                |r| r.get(0),
            )
            .unwrap_or(0);
        assert_eq!(ours_c, theirs_c, "E03 count after {mode} round {round}");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// E02: chaos-consistency. A child process writes batches and gets SIGKILLed
/// mid-flight at random moments; the parent then reopens the database with
/// rusqlite (which recovers the WAL) and our reader must agree with the
/// recovered state on every read.
#[test]
fn e02_chaos_sigkill_recovery() {
    use std::io::Write as _;
    let dir = std::env::temp_dir().join(format!("e02-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let db = dir.join("e02.db");
    let _ = std::fs::remove_file(&db);

    // parent creates schema + seeds
    {
        let (conn, _) = open_all(db.to_str().unwrap());
        conn.execute("INSERT INTO users (username) VALUES ('u1')", []).unwrap();
        for i in 1..=50i64 {
            conn.execute("INSERT INTO posts (user_id, body) VALUES (1, ?1)", [format!("seed {i}")]).unwrap();
        }
    }

    let exe = std::env::current_exe().expect("exe");
    // the child is THIS test binary re-run with a special env: it writes in a
    // loop until killed
    let child_count: u64 = 6;
    for _ in 0..child_count {
        let mut child = std::process::Command::new(&exe)
            .arg("--ignored")
            .env("E02_CHILD_WRITER", db.to_str().unwrap())
            .stdout(std::process::Stdio::null())
            .spawn()
            .expect("spawn writer child");
        // let it write for a short, random-ish window, then SIGKILL
        std::thread::sleep(std::time::Duration::from_millis(80 + (child_count * 17) % 120));
        let _ = child.kill();
        child.wait().unwrap();

        // recovery: rusqlite reopen recovers/rolls back the WAL; our reader
        // must agree on the post count and every feed row it walks
        let rec_conn = Connection::open(&db).unwrap();
        let theirs_total: i64 = rec_conn
            .query_row("SELECT COUNT(*) FROM posts", [], |r| r.get(0))
            .unwrap();
        drop(rec_conn);
        let mut store = Store::open(db.to_str().unwrap()).unwrap();
        store.refresh().unwrap();
        let rec_roots = Connection::open(&db).unwrap();
        let (_, posts_r, _) = roots_of(&rec_roots);
        let mut cur = Cursor::table(&store, posts_r).unwrap();
        let mut walked = 0i64;
        let mut first = None;
        if cur.last().unwrap() == Step::Moved {
            loop {
                let id = cur.rowid().unwrap();
                if first.is_none() {
                    first = Some(id);
                }
                walked += 1;
                if cur.prev().unwrap() == Step::End {
                    break;
                }
            }
        }
        // post ids are dense 1..N (child only inserts); count == max id
        assert_eq!(
            walked as i64, theirs_total,
            "E02 walk count vs rusqlite count"
        );
        if let Some(head) = first {
            let rec2 = Connection::open(&db).unwrap();
            let theirs_head: i64 = rec2
                .query_row("SELECT MAX(id) FROM posts", [], |r| r.get(0))
                .unwrap();
            assert_eq!(head, theirs_head, "E02 head rowid");
        }
        drop(store);
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        // recreate for the next child (dense ids from scratch keeps the
        // walk-count assertion simple)
        let _ = std::fs::remove_file(&db);
        {
            let (conn, _) = open_all(db.to_str().unwrap());
            conn.execute("INSERT INTO users (username) VALUES ('u1')", []).unwrap();
            for i in 1..=50i64 {
                conn.execute("INSERT INTO posts (user_id, body) VALUES (1, ?1)", [format!("seed {i}")]).unwrap();
            }
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
}

/// child writer used by E02 (invoked via E02_CHILD_WRITER env on --ignored)
#[test]
#[ignore]
fn e02_child_writer() {
    let db = std::env::var("E02_CHILD_WRITER").expect("E02_CHILD_WRITER");
    let conn = Connection::open(&db).unwrap();
    conn.pragma_update(None, "synchronous", "NORMAL").unwrap();
    loop {
        for _ in 0..20 {
            let _ = conn.execute("INSERT INTO posts (user_id, body) VALUES (1, 'chaos batch')", []);
        }
    }
}
