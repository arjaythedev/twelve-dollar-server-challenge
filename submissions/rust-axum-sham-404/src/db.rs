use std::env;
use std::time::Duration;

use rusqlite::{Connection, ErrorCode, params};
use serde::Serialize;

thread_local! {
    static CONN: Connection = {
        let db_path = env::var("SQLITE_PATH").unwrap_or_else(|_| "seed/feed.db".to_string());
        let conn = Connection::open(&db_path).expect("failed to open sqlite db");
        conn.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             PRAGMA foreign_keys = ON;
             PRAGMA busy_timeout = 30000;",
        )
        .expect("failed to set pragmas");
        conn
    };
}

#[derive(Serialize)]
pub struct PostItem {
    pub id: i64,
    pub body: String,
    pub created_at: String,
    pub author: String,
    pub like_count: i64,
}

pub enum LikeResult {
    Created,
    AlreadyLiked,
    PostNotFound,
}

/// The benchmark single-writer workload can still produce transient
/// `SQLITE_BUSY`/`SQLITE_LOCKED`; retry those instead of misreporting them.
fn is_busy(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(e, _)
            if matches!(e.code, ErrorCode::DatabaseBusy | ErrorCode::DatabaseLocked)
    )
}

fn is_constraint_violation(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(e, _) if e.code == ErrorCode::ConstraintViolation
    )
}

fn retry_on_busy<T>(mut f: impl FnMut() -> Result<T, rusqlite::Error>) -> Result<T, rusqlite::Error> {
    for attempt in 0..5 {
        match f() {
            Err(e) if is_busy(&e) && attempt < 4 => {
                std::thread::sleep(Duration::from_millis(1 << attempt));
            }
            other => return other,
        }
    }
    unreachable!()
}

pub fn ping() -> Result<(), rusqlite::Error> {
    CONN.with(|conn| conn.query_row("SELECT 1", [], |_| Ok(())))
}

pub fn get_20_feed() -> Result<Vec<PostItem>, rusqlite::Error> {
    CONN.with(|conn| {
        let mut stmt = conn.prepare_cached(
            "SELECT p.id, p.body, p.created_at, u.username, 
                    (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id)
             FROM posts p
             JOIN users u ON u.id = p.user_id
             ORDER BY p.created_at DESC, p.id DESC
             LIMIT 20",
        )?;

        let rows = stmt.query_map([], |row| {
            Ok(PostItem {
                id: row.get(0)?,
                body: row.get(1)?,
                created_at: row.get(2)?,
                author: row.get(3)?,
                like_count: row.get(4)?,
            })
        })?;
        rows.collect()
    })
}

pub fn get_post(id: i64) -> Result<Option<PostItem>, rusqlite::Error> {
    CONN.with(|conn| {
        let mut stmt = conn.prepare_cached(
            "SELECT p.id, p.body, p.created_at, u.username,
                    (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id)
             FROM posts p
             JOIN users u ON u.id = p.user_id
             WHERE p.id = ?1",
        )?;
        match stmt.query_row(params![id], |row| {
            Ok(PostItem {
                id: row.get(0)?,
                body: row.get(1)?,
                created_at: row.get(2)?,
                author: row.get(3)?,
                like_count: row.get(4)?,
            })
        }) {
            Ok(post) => Ok(Some(post)),
            Err(rusqlite::Error::QueryReturnedNoRows) => Ok(None),
            Err(e) => Err(e),
        }
    })
}

/// Returns the new post's `id` and the `created_at` value SQLite stored.
pub fn create_post(id: i64, body: &str) -> Result<(i64, String), rusqlite::Error> {
    CONN.with(|conn| {
        let mut stmt = conn.prepare_cached(
            "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
        )?;
        retry_on_busy(|| {
            stmt.query_row(params![id, body], |row| Ok((row.get(0)?, row.get(1)?)))
        })
    })
}

pub fn like_post(user_id: i64, post_id: i64) -> Result<LikeResult, rusqlite::Error> {
    CONN.with(|conn| {
        let mut stmt = conn.prepare_cached(
            "INSERT INTO likes (user_id, post_id)
             VALUES (?1, ?2)
             ON CONFLICT(user_id, post_id) DO NOTHING",
        )?;
        match retry_on_busy(|| stmt.execute(params![user_id, post_id])) {
            Ok(1) => Ok(LikeResult::Created),
            Ok(_) => Ok(LikeResult::AlreadyLiked),
            // A foreign-key violation is the only constraint that can fail
            // here, and it means the post (or user) does not exist.
            Err(e) if is_constraint_violation(&e) => Ok(LikeResult::PostNotFound),
            Err(e) => Err(e),
        }
    })
}
