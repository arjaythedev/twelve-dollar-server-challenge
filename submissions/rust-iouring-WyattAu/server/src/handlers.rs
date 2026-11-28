//! The five endpoints' data plane. Every read has two paths: the mmap
//! reader (hot) and the bundled-SQLite fallback (REQ-URIK-R7 degradation
//! ladder). The differential oracle proves path-1 against path-2.

use crate::store::{PostRow, Store};
use rusqlite::Connection;
use sqlite_btree::btree::{Cursor, Step, TableSeek};
use sqlite_btree::record::{decode_header, Field};
use sqlite_btree::scan::IndexScan;
use sqlite_btree::ReaderError;

const FIELDS: usize = 8;

fn de_err(e: sqlite_btree::DecodeError) -> ReaderError {
    match e {
        sqlite_btree::DecodeError::Truncated | sqlite_btree::DecodeError::VarintTooLong
        | sqlite_btree::DecodeError::SizeMismatch => ReaderError::Corrupt("record truncated"),
        other => ReaderError::Corrupt(match other {
            sqlite_btree::DecodeError::BadSerialType => "bad serial type",
            sqlite_btree::DecodeError::TooManyFields => "too many fields",
            _ => "record decode",
        })
    }
}

fn decode_fields(payload: &[u8]) -> Result<([Field; FIELDS], usize), ReaderError> {
    let mut f = [Field { serial_type: 0, offset: 0, len: 0 }; FIELDS];
    let (_, n) = decode_header(payload, &mut f).map_err(de_err)?;
    Ok((f, n))
}

fn field_int(p: &[u8], f: &Field) -> Option<i64> {
    let b = &p[f.offset..f.offset + f.len];
    Some(match f.serial_type {
        1 => b[0] as i8 as i64,
        2 => i16::from_be_bytes([b[0], b[1]]) as i64,
        3 => ((b[0] as i64) << 16) | ((b[1] as i64) << 8) | (b[2] as i64),
        4 => i32::from_be_bytes([b[0], b[1], b[2], b[3]]) as i64,
        5 => {
            let mut v = ((b[0] as i64) << 40)
                | ((b[1] as i64) << 32)
                | ((b[2] as i64) << 24)
                | ((b[3] as i64) << 16)
                | ((b[4] as i64) << 8)
                | (b[5] as i64);
            if b[0] & 0x80 != 0 {
                v |= !0i64 << 48;
            }
            v
        }
        6 => i64::from_be_bytes(b.try_into().ok()?),
        8 => 0,
        9 => 1,
        _ => return None,
    })
}

/// Decode a posts table record: (id=NULL [rowid alias], user_id, body,
/// created_at). The NULL slot for the rowid alias is SQLite's storage choice
/// (differential-oracle catch, 2026-11-22).
fn decode_post(payload: &[u8]) -> Result<(i64, Vec<u8>, Vec<u8>), ReaderError> {
    let (f, n) = decode_fields(payload)?;
    if n < 4 || f[0].serial_type != 0 {
        return Err(ReaderError::Corrupt("unexpected posts record shape"));
    }
    let user_id = field_int(payload, &f[1]).ok_or(ReaderError::Corrupt("user_id type"))?;
    Ok((
        user_id,
        payload[f[2].offset..f[2].offset + f[2].len].to_vec(),
        payload[f[3].offset..f[3].offset + f[3].len].to_vec(),
    ))
}

/// users record: (id=NULL, username, created_at)
fn decode_username(payload: &[u8]) -> Result<Vec<u8>, ReaderError> {
    let (f, n) = decode_fields(payload)?;
    if n < 2 || f[0].serial_type != 0 {
        return Err(ReaderError::Corrupt("unexpected users record shape"));
    }
    Ok(payload[f[1].offset..f[1].offset + f[1].len].to_vec())
}

// ---- reader-path reads -----------------------------------------------------

/// One resolved feed/post entry.
pub struct PostFull {
    pub id: i64,
    pub user_id: i64,
    pub body: Vec<u8>,
    pub created_at: Vec<u8>,
    pub author: Vec<u8>,
    pub like_count: i64,
}

/// Feed head via backward walk over the posts table (ADR 0002), with author
/// resolve and like counts per row. JOIN semantics: a post without a user
/// row is dropped (SPEC reference SQL joins users).
pub fn reader_feed(store: &Store, max: usize) -> Result<Vec<PostFull>, ReaderError> {
    let r = store.reader().ok_or(ReaderError::OutOfRange)?;
    let roots = &store.roots;
    let mut cur = Cursor::table(r, roots.posts)?;
    let mut out = Vec::with_capacity(max);
    if cur.last()? != Step::Moved {
        return Ok(out);
    }
    loop {
        let id = cur.rowid()?;
        let payload = cur.payload()?;
        let (user_id, body, created) = decode_post(payload)?;
        let mut ucur = Cursor::table(r, roots.users)?;
        let username = match ucur.table_seek(user_id)? {
            TableSeek::Found => decode_username(ucur.payload()?)?,
            // JOIN drops author-less posts
            _ => {
                if cur.prev()? != Step::Moved {
                    return Ok(out);
                }
                continue;
            }
        };
        let mut scan = IndexScan::new(r, roots.likes_idx)?;
        let like_count = scan.count_prefix(id)?;
        out.push(PostFull {
            id,
            user_id,
            body: body.clone(),
            created_at: created.clone(),
            author: username,
            like_count,
        });
        if out.len() == max || cur.prev()? != Step::Moved {
            return Ok(out);
        }
    }
}

/// Like count for one post via the in-order index scanner.
pub fn reader_like_count(store: &Store, post_id: i64) -> Result<i64, ReaderError> {
    let r = store.reader().ok_or(ReaderError::OutOfRange)?;
    let mut scan = IndexScan::new(r, store.roots.likes_idx)?;
    scan.count_prefix(post_id)
}

/// Single post by id via table seek.
pub fn reader_post(store: &Store, id: i64) -> Result<Option<PostRow>, ReaderError> {
    let r = store.reader().ok_or(ReaderError::OutOfRange)?;
    let mut cur = Cursor::table(r, store.roots.posts)?;
    if cur.table_seek(id)? != TableSeek::Found {
        return Ok(None);
    }
    let payload = cur.payload()?;
    let (user_id, body, created) = decode_post(payload)?;
    Ok(Some(PostRow { id, user_id, body, created_at: created }))
}

/// Author username for a user_id (users table seek).
pub fn reader_username(store: &Store, user_id: i64) -> Result<Option<Vec<u8>>, ReaderError> {
    let r = store.reader().ok_or(ReaderError::OutOfRange)?;
    let mut cur = Cursor::table(r, store.roots.users)?;
    if cur.table_seek(user_id)? != TableSeek::Found {
        return Ok(None);
    }
    decode_username(cur.payload()?).map(Some)
}

/// Zero-copy feed emitter (E18): walks the posts table backward and writes
/// the SPEC feed JSON straight into `out`, pulling body/created_at bytes as
/// views into the mmap and resolving authors/counts per row. No PostFull
/// intermediates. JOIN semantics preserved (posts without users drop).
pub fn reader_feed_emit(store: &Store, max: usize, out: &mut Vec<u8>) -> Result<(), ReaderError> {
    let r = store.reader().ok_or(ReaderError::OutOfRange)?;
    let roots = &store.roots;
    let mut cur = Cursor::table(r, roots.posts)?;
    out.reserve(4096);
    out.extend_from_slice(b"{\"posts\":[");
    let mut emitted = 0usize;
    if cur.last()? != Step::Moved {
        out.push(b']');
        out.push(b'}');
        return Ok(());
    }
    let mut ucur = Cursor::table(r, roots.users)?;
    let mut scan = IndexScan::new(r, roots.likes_idx)?;
    loop {
        let id = cur.rowid()?;
        let payload = cur.payload()?;
        let (f, nf) = {
            let mut buf = [Field { serial_type: 0, offset: 0, len: 0 }; 8];
            let (_, n) = decode_header(payload, &mut buf).map_err(|_| ReaderError::Corrupt("posts record"))?;
            (buf, n)
        };
        if nf < 4 || f[0].serial_type != 0 {
            return Err(ReaderError::Corrupt("unexpected posts record shape"));
        }
        let user_id = {
            let b = &payload[f[1].offset..f[1].offset + f[1].len];
            match f[1].serial_type {
                1 => b[0] as i8 as i64,
                2 => i16::from_be_bytes([b[0], b[1]]) as i64,
                3 => ((b[0] as i64) << 16) | ((b[1] as i64) << 8) | (b[2] as i64),
                4 => i32::from_be_bytes([b[0], b[1], b[2], b[3]]) as i64,
                8 => 0,
                9 => 1,
                _ => return Err(ReaderError::Corrupt("user_id type")),
            }
        };
        let created = &payload[f[3].offset..f[3].offset + f[3].len];
        // author resolve (users seek by rowid)
        if ucur.table_seek(user_id)? != TableSeek::Found {
            // JOIN drops author-less posts
            if cur.prev()? != Step::Moved {
                break;
            }
            continue;
        }
        let username = {
            let up = ucur.payload()?;
            let (uf, unf) = {
                let mut ub = [Field { serial_type: 0, offset: 0, len: 0 }; 4];
                let (_, un) = decode_header(up, &mut ub).map_err(|_| ReaderError::Corrupt("users record"))?;
                (ub, un)
            };
            if unf < 2 {
                return Err(ReaderError::Corrupt("users record short"));
            }
            &up[uf[1].offset..uf[1].offset + uf[1].len]
        };
        let like_count = scan.count_prefix(id)?;
        // emit row: {"id":N,"body":"…","created_at":"…","author":"…","like_count":N}
        if emitted > 0 {
            out.push(b',');
        }
        out.extend_from_slice(b"{\"id\":");
        let mut num = [0u8; 20];
        let nd = uring_http::json::write_decimal(&mut num, 0, id);
        out.extend_from_slice(&num[..nd]);
        out.extend_from_slice(b",\"body\":\"");
        // JSON-escape (quotes/control chars); SPEC passes <>&/ and non-ASCII raw
        let mut esc = [0u8; 2048];
        let nesc = uring_http::json::write_escaped(&mut esc, &payload[f[2].offset..f[2].offset + f[2].len]);
        out.extend_from_slice(&esc[..nesc]);
        out.extend_from_slice(b"\",\"created_at\":\"");
        out.extend_from_slice(created);
        out.extend_from_slice(b"\",\"author\":\"");
        out.extend_from_slice(username);
        out.extend_from_slice(b"\",\"like_count\":");
        let nd2 = uring_http::json::write_decimal(&mut num, 0, like_count);
        out.extend_from_slice(&num[..nd2]);
        out.push(b'}');
        emitted += 1;
        if emitted == max || cur.prev()? != Step::Moved {
            break;
        }
    }
    out.extend_from_slice(b"]}");
    Ok(())
}

/// Zero-copy single-post GET (E19): seeks the posts row, resolves the
/// author, counts likes, and writes the SPEC `{"post":{...}}` body straight
/// into `out` from mmap views. No PostRow intermediates. Byte-parity with
/// the fallback composite path (write_post_json format; author escaped).
/// Ok(true) = emitted, Ok(false) = post or author not found.
pub fn reader_post_get_emit(
    store: &Store,
    id: i64,
    out: &mut Vec<u8>,
) -> Result<bool, ReaderError> {
    let r = store.reader().ok_or(ReaderError::OutOfRange)?;
    let mut cur = Cursor::table(r, store.roots.posts)?;
    if cur.table_seek(id)? != TableSeek::Found {
        return Ok(false);
    }
    let payload = cur.payload()?;
    let (f, nf) = decode_fields(payload)?;
    if nf < 4 || f[0].serial_type != 0 {
        return Err(ReaderError::Corrupt("unexpected posts record shape"));
    }
    let user_id = field_int(payload, &f[1]).ok_or(ReaderError::Corrupt("user_id type"))?;
    let body = &payload[f[2].offset..f[2].offset + f[2].len];
    let created = &payload[f[3].offset..f[3].offset + f[3].len];
    // author resolve (JOIN semantics: author-less posts 404 like the feed drop)
    let mut ucur = Cursor::table(r, store.roots.users)?;
    if ucur.table_seek(user_id)? != TableSeek::Found {
        return Ok(false);
    }
    let up = ucur.payload()?;
    let (uf, unf) = {
        let mut ub = [Field { serial_type: 0, offset: 0, len: 0 }; 4];
        let (_, un) = decode_header(up, &mut ub)
            .map_err(|_| ReaderError::Corrupt("users record"))?;
        (ub, un)
    };
    if unf < 2 {
        return Err(ReaderError::Corrupt("users record short"));
    }
    let username = &up[uf[1].offset..uf[1].offset + uf[1].len];
    let mut scan = IndexScan::new(r, store.roots.likes_idx)?;
    let like_count = scan.count_prefix(id)?;
    // emit {"post":{...}} — write_post_json byte format
    out.extend_from_slice(b"{\"post\":{\"id\":");
    let mut num = [0u8; 20];
    let mut nd = uring_http::json::write_decimal(&mut num, 0, id);
    out.extend_from_slice(&num[..nd]);
    out.extend_from_slice(b",\"body\":\"");
    let mut esc = [0u8; 2048];
    let nesc = uring_http::json::write_escaped(&mut esc, body);
    out.extend_from_slice(&esc[..nesc]);
    out.extend_from_slice(b"\",\"created_at\":\"");
    out.extend_from_slice(created);
    out.extend_from_slice(b"\",\"author\":\"");
    let na = uring_http::json::write_escaped(&mut esc, username);
    out.extend_from_slice(&esc[..na]);
    out.extend_from_slice(b"\",\"like_count\":");
    nd = uring_http::json::write_decimal(&mut num, 0, like_count);
    out.extend_from_slice(&num[..nd]);
    out.extend_from_slice(b"}}");
    Ok(true)
}

// ---- fallback-path reads (bundled SQLite) -----------------------------------

pub fn fallback_post(conn: &Connection, id: i64) -> Option<PostRow> {
    conn.query_row(
        "SELECT id, user_id, body, created_at FROM posts WHERE id = ?1",
        [id],
        |r| {
            Ok(PostRow {
                id: r.get(0).unwrap(),
                user_id: r.get(1).unwrap(),
                body: r.get::<_, String>(2).unwrap_or_default().into_bytes(),
                created_at: r.get::<_, String>(3).unwrap_or_default().into_bytes(),
            })
        },
    )
    .ok()
}

pub fn fallback_username(conn: &Connection, user_id: i64) -> Option<Vec<u8>> {
    conn.query_row("SELECT username FROM users WHERE id = ?1", [user_id], |r| {
        r.get::<_, String>(0)
    })
    .ok()
    .map(|s| s.into_bytes())
}

pub fn fallback_like_count(conn: &Connection, post_id: i64) -> Option<i64> {
    conn.query_row(
        "SELECT COUNT(*) FROM likes WHERE post_id = ?1",
        [post_id],
        |r| r.get(0),
    )
    .ok()
}

/// SPEC reference query (SPEC.md), used by the feed fallback path.
pub fn fallback_feed(conn: &Connection, max: usize) -> Vec<PostFull> {
    let mut st = match conn.prepare(
        "SELECT p.id, p.user_id, p.body, p.created_at, u.username, \
         (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id) \
         FROM posts p JOIN users u ON u.id = p.user_id \
         ORDER BY p.created_at DESC, p.id DESC LIMIT ?1",
    ) {
        Ok(st) => st,
        Err(_) => return Vec::new(),
    };
    let mut rows = match st.query([max as i64]) {
        Ok(rows) => rows,
        Err(_) => return Vec::new(),
    };
    let mut out = Vec::new();
    while let Some(r) = rows.next().unwrap_or(None) {
        out.push(PostFull {
            id: r.get(0).unwrap_or_default(),
            user_id: r.get(1).unwrap_or_default(),
            body: r.get::<_, String>(2).unwrap_or_default().into_bytes(),
            created_at: r.get::<_, String>(3).unwrap_or_default().into_bytes(),
            author: r.get::<_, String>(4).unwrap_or_default().into_bytes(),
            like_count: r.get(5).unwrap_or_default(),
        });
    }
    out
}

// ---- writes (bundled SQLite; group commit is the pass layer's job) ----------

pub fn insert_post(conn: &Connection, user_id: i64, body: &[u8]) -> Result<(i64, Vec<u8>), String> {
    conn.execute(
        "INSERT INTO posts (user_id, body) VALUES (?1, ?2)",
        rusqlite::params![user_id, body],
    )
    .map_err(|e| e.to_string())?;
    let id = conn.last_insert_rowid();
    let created: Vec<u8> = conn
        .query_row(
            "SELECT created_at FROM posts WHERE id = ?1",
            [id],
            |r| r.get::<_, String>(0),
        )
        .map(|s| s.into_bytes())
        .map_err(|e| e.to_string())?;
    Ok((id, created))
}

/// true = first like (201), false = already liked (200).
pub fn insert_like(conn: &Connection, user_id: i64, post_id: i64) -> Result<bool, String> {
    conn.execute(
        "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
        rusqlite::params![user_id, post_id],
    )
    .map_err(|e| e.to_string())?;
    Ok(conn.changes() > 0)
}

// ---- unified read surface with the degradation ladder -----------------------

pub fn read_post(store: &Store, conn: &Connection, id: i64) -> Option<PostRow> {
    match reader_post(store, id) {
        Ok(v) => v,
        Err(_) => {
            store.note_fallback();
            fallback_post(conn, id)
        }
    }
}

pub fn read_like_count(store: &Store, conn: &Connection, post_id: i64) -> Option<i64> {
    match reader_like_count(store, post_id) {
        Ok(v) => Some(v),
        Err(_) => {
            store.note_fallback();
            fallback_like_count(conn, post_id)
        }
    }
}

pub fn read_username(store: &Store, conn: &Connection, user_id: i64) -> Option<Vec<u8>> {
    match reader_username(store, user_id) {
        Ok(v) => v,
        Err(_) => {
            store.note_fallback();
            fallback_username(conn, user_id)
        }
    }
}

pub fn read_feed(store: &Store, conn: &Connection, max: usize) -> Vec<PostFull> {
    match reader_feed(store, max) {
        Ok(v) => v,
        Err(_) => {
            store.note_fallback();
            fallback_feed(conn, max)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn setup() -> (Connection, Store, String) {
        let dir = std::env::temp_dir().join(format!("handlers-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let db = dir.join("handlers.db");
        let _ = std::fs::remove_file(&db);
        let conn = Connection::open(&db).unwrap();
        conn.pragma_update(None, "journal_mode", "WAL").unwrap();
        conn.pragma_update(None, "synchronous", "NORMAL").unwrap();
        conn.pragma_update(None, "wal_autocheckpoint", 0).unwrap();
        conn.execute_batch(
            "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
             CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
             CREATE TABLE likes (user_id INTEGER NOT NULL, post_id INTEGER NOT NULL, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), PRIMARY KEY (user_id, post_id));
             CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
             CREATE INDEX likes_post_id_idx ON likes (post_id);",
        )
        .unwrap();
        for i in 1..=10i64 {
            conn.execute("INSERT INTO users (username) VALUES (?1)", [format!("u{i}")]).unwrap();
        }
        for i in 1..=50i64 {
            conn.execute(
                "INSERT INTO posts (user_id, body) VALUES (?1, ?2)",
                rusqlite::params![i % 10 + 1, format!("body {i}")],
            )
            .unwrap();
        }
        for i in 1..=20i64 {
            conn.execute(
                "INSERT OR IGNORE INTO likes (user_id, post_id) VALUES (?1, ?2)",
                [(i % 10) + 1, (i * 2) % 50 + 1],
            )
            .unwrap();
        }
        let store = Store::open(db.to_str().unwrap(), "twelve-dollar-challenge").unwrap();
        (conn, store, db.to_string_lossy().into_owned())
    }

    #[test]
    fn reader_and_fallback_agree() {
        let (conn, mut store, _dir) = setup();
        store.refresh_snapshot();
        for id in 1..=50i64 {
            let via_reader = reader_post(&store, id).unwrap();
            let via_fallback = fallback_post(&conn, id);
            assert_eq!(via_reader.is_some(), via_fallback.is_some(), "seek id {id}");
            if let (Some(a), Some(b)) = (&via_reader, &via_fallback) {
                assert_eq!(a.user_id, b.user_id, "id {id}");
                assert_eq!(a.body, b.body, "id {id}");
            }
            let rc = reader_like_count(&store, id).unwrap();
            let fc = fallback_like_count(&conn, id).unwrap();
            assert_eq!(rc, fc, "like count id {id}");
            if let Some(p) = &via_reader {
                let ru = reader_username(&store, p.user_id).unwrap();
                let fu = fallback_username(&conn, p.user_id);
                assert_eq!(ru, fu, "username for user {}", p.user_id);
            }
        }
        let rf = reader_feed(&store, 20).unwrap();
        let ff = fallback_feed(&conn, 20);
        assert_eq!(rf.len(), ff.len(), "feed len");
        for (a, b) in rf.iter().zip(ff.iter()) {
            assert_eq!(a.id, b.id, "feed order");
            assert_eq!(a.author, b.author, "feed author");
            assert_eq!(a.like_count, b.like_count, "feed like_count");
            assert_eq!(a.body, b.body, "feed body");
        }
        // write path: insert a post, refresh, it appears at the feed head
        let (id, _created) = insert_post(&conn, 3, b"new head post").unwrap();
        store.refresh_snapshot();
        let head = reader_feed(&store, 1).unwrap();
        assert_eq!(head.len(), 1);
        assert_eq!(head[0].id, id, "new post at feed head after commit+refresh");
        assert_eq!(head[0].body, b"new head post");
        // like it via writer, refresh, count updates
        insert_like(&conn, 5, id).unwrap();
        store.refresh_snapshot();
        let lc = reader_like_count(&store, id).unwrap();
        assert_eq!(lc, 1, "fresh like visible after commit+refresh");
        let _ = std::fs::remove_dir_all(std::env::temp_dir().join(format!("handlers-{}", std::process::id())));
    }
}
