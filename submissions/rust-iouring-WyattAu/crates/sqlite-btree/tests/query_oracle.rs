//! Differential oracle (G2): our reader must agree with SQLite on every
//! read pattern, on every snapshot, including after WAL growth and
//! checkpoint truncation. Reference implementation: rusqlite (bundled
//! amalgamation). Harness pattern: WyattAu/audiobook-conformance.
//!
//! Corpus: a synthetic database with the challenge schema plus, when
//! `ORACLE_SEED` points at it, the real 238 MB seed (read-only comparisons).

use rusqlite::Connection;
use sqlite_btree::btree::{Cursor, Step, TableSeek};
use sqlite_btree::record::{decode_header, Field};
use sqlite_btree::store::Store;

const SCHEMA: &str = r#"
CREATE TABLE users (
  id          INTEGER PRIMARY KEY,
  username    TEXT NOT NULL UNIQUE,
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE posts (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE likes (
  user_id     INTEGER NOT NULL REFERENCES users(id),
  post_id     INTEGER NOT NULL REFERENCES posts(id),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  PRIMARY KEY (user_id, post_id)
);
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX likes_post_id_idx ON likes (post_id);
"#;

struct Roots {
    users: u32,
    posts: u32,
    likes_idx: u32,
}

fn roots_of(conn: &Connection) -> Roots {
    let mut st = conn
        .prepare("SELECT name, rootpage FROM sqlite_schema WHERE name IN ('users','posts','likes_post_id_idx')")
        .unwrap();
    let mut rows = st.query([]).unwrap();
    let mut roots = Roots { users: 0, posts: 0, likes_idx: 0 };
    while let Some(r) = rows.next().unwrap() {
        let name: String = r.get(0).unwrap();
        let page: i64 = r.get(1).unwrap();
        match name.as_str() {
            "users" => roots.users = page as u32,
            "posts" => roots.posts = page as u32,
            "likes_post_id_idx" => roots.likes_idx = page as u32,
            _ => {}
        }
    }
    assert!(roots.users > 0 && roots.posts > 0 && roots.likes_idx > 0);
    roots
}

/// Decode record fields 0..k into (int, bytes...) views per the record header.
fn fields<'a>(payload: &'a [u8], out: &'a mut [Field; 8]) -> &'a [Field] {
    let (_, n) = decode_header(payload, out).expect("record header decodes");
    &out[..n]
}

fn field_bytes<'a>(payload: &'a [u8], f: &Field) -> &'a [u8] {
    &payload[f.offset..f.offset + f.len]
}

fn field_int(payload: &[u8], f: &Field) -> i64 {
    let b = &payload[f.offset..f.offset + f.len];
    match f.serial_type {
        1 => b[0] as i8 as i64,
        2 => i16::from_be_bytes([b[0], b[1]]) as i64,
        3 => ((b[0] as i64) << 16) | ((b[1] as i64) << 8) | (b[2] as i64),
        4 => i32::from_be_bytes([b[0], b[1], b[2], b[3]]) as i64,
        8 => 0,
        9 => 1,
        _ => panic!("unexpected int serial type {}", f.serial_type),
    }
}

#[test]
fn reader_agrees_with_sqlite() -> Result<(), Box<dyn std::error::Error>> {
    let dir = std::env::temp_dir().join(format!("oracle-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let db_path = dir.join("oracle.db");
    let _ = std::fs::remove_file(&db_path);
    let _ = std::fs::remove_file(dir.join("oracle.db-wal"));
    let _ = std::fs::remove_file(dir.join("oracle.db-shm"));

    let conn = Connection::open(&db_path).unwrap();
    conn.pragma_update(None, "journal_mode", "WAL").unwrap();
    conn.pragma_update(None, "synchronous", "NORMAL").unwrap();
    conn.pragma_update(None, "wal_autocheckpoint", 0).unwrap();
    conn.execute_batch(SCHEMA).unwrap();

    // seed: 200 users, 1000 posts, ~800 likes with skewed like distribution
    {
        let mut ins_u = conn.prepare("INSERT INTO users (username) VALUES (?1)").unwrap();
        for i in 0..200usize {
            ins_u.execute([format!("user{i}")]).unwrap();
        }
        let mut ins_p = conn.prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2)").unwrap();
        for i in 0..1000usize {
            let user = (i * 37 + 11) % 200 + 1;
            let body = format!("post body {i} with some content");
            ins_p.execute(rusqlite::params![user, body]).unwrap();
        }
        let mut ins_l = conn.prepare("INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)").unwrap();
        for i in 0..800usize {
            let post = (i * 53 + 7) % 1000 + 1;
            let user = (i * 91 + 3) % 200 + 1;
            ins_l.execute([user as i64, post as i64]).unwrap();
        }
    }

    let roots = roots_of(&conn);
    {
        use std::io::Read as _; use memmap2::Mmap as _MmapMarker;
        let mut f = std::fs::File::open(&db_path).unwrap();
        let mut h = vec![0u8; 100];
        f.read_exact(&mut h).unwrap();
        eprintln!("hdr ps={} enc={:?}", u16::from_be_bytes([h[16], h[17]]), &h[56..60]);
        let m = unsafe { memmap2::Mmap::map(&std::fs::File::open(&db_path).unwrap()).unwrap() };
        eprintln!("mmap enc={:?}", &m[56..60]);
    }
    let mut store = Store::open(&db_path).expect("store opens");
    {
        let wl = std::fs::metadata(dir.join("oracle.db-wal")).map(|m| m.len()).unwrap_or(0);
        eprintln!("at open: snap={} wal_len={wl} db_len={}", store.snapshot(), std::fs::metadata(&db_path).map(|m| m.len()).unwrap_or(0));
    }

    // deterministic pseudo-random driver
    let mut seed = 0x1234_5678u64;
    let mut rng = move || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };

    for round in 0..12u64 {
        // -- write batch through the reference writer (rusqlite) --
        let new_posts: u64 = rng() % 20;
        let new_likes: u64 = rng() % 40;
        {
            let mut ins_p = conn.prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2)").unwrap();
            for _ in 0..new_posts {
                let user = (rng() % 200) as i64 + 1;
                let body = format!("round {round} body {}", rng() % 1000);
                ins_p.execute(rusqlite::params![user, body]).unwrap();
            }
            let max_post: i64 = conn.query_row("SELECT COALESCE(MAX(id),0) FROM posts", [], |r| r.get(0)).unwrap();
            let mut ins_l = conn.prepare("INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)").unwrap();
            for _ in 0..new_likes {
                let post = (rng() % max_post as u64) as i64 + 1;
                let user = (rng() % 200) as i64 + 1;
                ins_l.execute([user, post]).unwrap();
            }
        }

        // -- advance our reader's snapshot --
        store.refresh().unwrap();

        // -- compare table seeks --
        for _ in 0..40 {
            let id = (rng() % (1000 + round * 20 + 5)) as i64 + 1;
            let theirs = conn
                .query_row(
                    "SELECT user_id, body, created_at FROM posts WHERE id = ?1",
                    [id],
                    |r| Ok((r.get::<_, i64>(0).unwrap(), r.get::<_, String>(1).unwrap(), r.get::<_, String>(2).unwrap())),
                )
                .ok();
            let store_state = format!("snap={} pages_dbg={store:?}", store.snapshot());
            let mut cur = Cursor::table(&store, roots.posts).unwrap();
            let ours = match cur.table_seek(id).map_err(|e| panic!("seek id {id}: {e:?} round {round} state {store_state}"))? {
                TableSeek::Found => {
                    let p = cur.payload().map_err(|e| panic!("payload id {id}: {e:?}"))?;
                    let mut f = [Field { serial_type: 0, offset: 0, len: 0 }; 8];
                    let (_, _nfields) = decode_header(p, &mut f).unwrap();
                    // posts record: (id=NULL [rowid alias], user_id, body, created_at)
                    // SQLite stores NULL in the record slot of an INTEGER
                    // PRIMARY KEY column; the rowid carries the value.
                    let user = field_int(p, &f[1]);
                    let body = String::from_utf8_lossy(field_bytes(p, &f[2])).into_owned();
                    let created = String::from_utf8_lossy(field_bytes(p, &f[3])).into_owned();
                    Some((user, body, created))
                }
                _ => None,
            };
            assert_eq!(ours, theirs, "table seek mismatch for id {id} round {round}");
        }

        // -- compare like counts --
        for _ in 0..30 {
            let id = (rng() % (1000 + round * 20 + 5)) as i64 + 1;
            let theirs: i64 = conn
                .query_row("SELECT COUNT(*) FROM likes WHERE post_id = ?1", [id], |r| r.get(0))
                .unwrap();
            let mut scan = sqlite_btree::scan::IndexScan::new(&store, roots.likes_idx).unwrap();
            let ours = scan.count_prefix(id).unwrap();
            if ours != theirs {
                let mut dbg = sqlite_btree::scan::IndexScan::new(&store, roots.likes_idx).unwrap();
                dbg.lower_bound(id.saturating_sub(2)).unwrap();
                let mut ks: Vec<i64> = Vec::new();
                for _ in 0..160 {
                    match dbg.advance().unwrap() {
                        Some(p) => ks.push(sqlite_btree::scan::IndexScan::key_of_pub(p)),
                        None => { ks.push(i64::MIN); break; }
                    }
                }
                panic!("dump keys around {id}: {ks:?} ours_count={ours}");
            }
            if ours != theirs {
                store.refresh().unwrap();
                let mut scan2 = sqlite_btree::scan::IndexScan::new(&store, roots.likes_idx).unwrap();
                let recount = scan2.count_prefix(id).unwrap();
                let mut scan3 = sqlite_btree::scan::IndexScan::new(&store, roots.likes_idx).unwrap();
                scan3.lower_bound(500).unwrap();
                let mut ours_ks = Vec::new();
                while let Some(p) = scan3.advance().unwrap() {
                    let k = sqlite_btree::scan::IndexScan::key_of_pub(p);
                    if k > 510 { break; }
                    ours_ks.push(k);
                }
                panic!("like count mismatch id {id} round {round}: ours={ours} theirs={theirs} recount={recount} ours_500_510={ours_ks:?} snap={}", store.snapshot());
            }
        }

        // -- compare feed head (backward walk over posts) --
        {
            let wl = std::fs::metadata(dir.join("oracle.db-wal")).map(|m| m.len()).unwrap_or(0);
            eprintln!("round {round} post-refresh: snap={} wal_len={wl} db_len={}", store.snapshot(), std::fs::metadata(&db_path).map(|m| m.len()).unwrap_or(0));
            let wl = std::fs::metadata(dir.join("oracle.db-wal")).map(|m| m.len()).unwrap_or(0);
            let mut wf = std::fs::File::open(dir.join("oracle.db-wal")).unwrap();
            let mut wh = [0u8; 32];
            use std::io::Read as _;
            let _ = wf.read_exact(&mut wh);
            let ac: i64 = conn.query_row("PRAGMA wal_autocheckpoint", [], |r| r.get(0)).unwrap_or(-1);
            let jm: String = conn.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap_or("?".into());
            eprintln!("round {round} after refresh: snap={} wal_len={wl} hdr_salt1={} ac={ac} jm={jm}", store.snapshot(), u32::from_be_bytes([wh[16], wh[17], wh[18], wh[19]]));
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
            let mut cur = Cursor::table(&store, roots.posts).unwrap();
            let mut ours = Vec::new();
            if cur.last().unwrap() == Step::Moved {
                loop {
                    ours.push(cur.rowid().unwrap());
                    if ours.len() == 20 || cur.prev().unwrap() == Step::End {
                        break;
                    }
                }
            }
            // NOTE: this comparison assumes the created_at/id order equivalence
            // (ADR 0002 invariant). The synthetic writer inserts with default
            // timestamps in increasing rounds; same-ms ties break by id DESC,
            // and the invariant check would be exercised by the server path.
            assert_eq!(ours, theirs, "feed head mismatch round {round}");
        }

        // -- checkpoint round: every 3rd round, truncate the WAL and re-check --
        if round % 3 == 2 {
            conn.pragma_update(None, "wal_checkpoint", "TRUNCATE").unwrap();
            store.refresh().unwrap();
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
            let mut cur = Cursor::table(&store, roots.posts).unwrap();
            let mut seen = Vec::new();
            if cur.last().unwrap() == Step::Moved {
                loop {
                    seen.push(cur.rowid().unwrap());
                    if seen.len() == 20 || cur.prev().unwrap() == Step::End {
                        break;
                    }
                }
            }
            assert_eq!(seen, theirs, "post-checkpoint feed head round {round}");
        }
    }

    // real-scale validation: the actual challenge seed, read-only
    if let Ok(seed_path) = std::env::var("ORACLE_SEED") {
        let mut store = Store::open(&seed_path).expect("seed store opens");
        let conn = Connection::open_with_flags(
            &seed_path,
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
        )
        .unwrap();
        let roots = roots_of(&conn);
        // feed head
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
        let mut cur = Cursor::table(&store, roots.posts).unwrap();
        let mut ours = Vec::new();
        if cur.last().unwrap() == Step::Moved {
            loop {
                ours.push(cur.rowid().unwrap());
                if ours.len() == 20 || cur.prev().unwrap() == Step::End {
                    break;
                }
            }
        }
        assert_eq!(ours, theirs, "seed feed head");
        // random seeks + like counts across the full id range
        let mut seed = 0xdead_beefu64;
        let mut rng = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        for _ in 0..300 {
            let id = (rng() % 500_000) as i64 + 1;
            let mut cur = Cursor::table(&store, roots.posts).unwrap();
            let ours_found = cur.table_seek(id).unwrap() == TableSeek::Found;
            let theirs: bool = conn
                .query_row("SELECT 1 FROM posts WHERE id = ?1", [id], |_| Ok(true))
                .is_ok();
            assert_eq!(ours_found, theirs, "seed seek id {id}");
        }
        for _ in 0..100 {
            let id = (rng() % 500_000) as i64 + 1;
            let mut scan = sqlite_btree::scan::IndexScan::new(&store, roots.likes_idx).unwrap();
            let ours = scan.count_prefix(id).unwrap();
            let theirs: i64 = conn
                .query_row("SELECT COUNT(*) FROM likes WHERE post_id = ?1", [id], |r| r.get(0))
                .unwrap();
            if ours != theirs {
                let mut dbg = sqlite_btree::scan::IndexScan::new(&store, roots.likes_idx).unwrap();
                dbg.lower_bound(id.saturating_sub(1)).unwrap();
                let mut ks: Vec<(i64, i64)> = Vec::new();
                for _ in 0..140 {
                    match dbg.advance().unwrap() {
                        Some(p) => ks.push(sqlite_btree::scan::IndexScan::key_row_pub(p)),
                        None => { ks.push((i64::MIN, i64::MIN)); break; }
                    }
                }
                let their_rows: Vec<(i64, i64)> = {
                    let mut st = conn
                        .prepare("SELECT post_id, rowid FROM likes WHERE post_id BETWEEN ?1 AND ?2 ORDER BY post_id, rowid")
                        .unwrap();
                    let mut rows = st.query([id.saturating_sub(1), id + 1]).unwrap();
                    let mut v = Vec::new();
                    while let Some(r) = rows.next().unwrap() {
                        v.push((r.get::<_, i64>(0).unwrap(), r.get::<_, i64>(1).unwrap()));
                    }
                    v
                };
                panic!("dump id {id}: ours_ks={ks:?} their_rows={their_rows:?}");
            }
            assert_eq!(ours, theirs, "seed like count id {id}");
        }
        eprintln!("seed oracle: 300 seeks + 100 counts + feed head all agree");
    }

    let _ = std::fs::remove_dir_all(&dir);
    Ok(())
}

fn theirs_len(conn: &Connection) -> usize {
    conn.query_row("SELECT COUNT(*) FROM posts", [], |r| r.get::<_, i64>(0)).unwrap() as usize
}
