//! Single-owner SQLite actor.
//!
//! One dedicated thread owns the connection and prepared-statement cache. HTTP
//! tasks send commands over a bounded channel and await a oneshot reply, so no
//! database work ever runs on the async runtime. The actor drains every command
//! that is already queued (up to `BATCH_MAX`), executes reads immediately, and
//! commits the batch's writes in one transaction before answering any of them.
//! A low-priority thread performs PASSIVE WAL checkpoints off the request path.

use std::thread;
use std::time::Duration;

use rusqlite::{CachedStatement, Connection, ErrorCode, params, types::ValueRef};
use tokio::sync::{mpsc, oneshot};

use crate::json::{self, Writer};

const COMMAND_QUEUE: usize = 4096;
const BATCH_MAX: usize = 256;
const CHECKPOINT_INTERVAL: Duration = Duration::from_millis(100);

const FEED_SQL: &str = "
    SELECT p.id, p.body, p.created_at, u.username,
           (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id)
      FROM posts p
      JOIN users u ON u.id = p.user_id
     ORDER BY p.created_at DESC, p.id DESC
     LIMIT 20";

const GET_POST_SQL: &str = "
    SELECT p.id, p.body, p.created_at, u.username,
           (SELECT COUNT(*) FROM likes l WHERE l.post_id = p.id)
      FROM posts p
      JOIN users u ON u.id = p.user_id
     WHERE p.id = ?1";

const CREATE_POST_SQL: &str = "
    INSERT INTO posts (user_id, body) VALUES (?1, ?2)
    RETURNING id, created_at";

const LIKE_SQL: &str = "
    INSERT INTO likes (user_id, post_id) VALUES (?1, ?2)
    ON CONFLICT (user_id, post_id) DO NOTHING";

#[derive(Debug, Clone, Copy)]
pub enum DbError {
    Unavailable,
}

pub enum LikeOutcome {
    Created(Vec<u8>),
    AlreadyLiked(Vec<u8>),
    PostNotFound,
}

type Reply<T> = oneshot::Sender<T>;

enum Command {
    Feed(Reply<Result<Vec<u8>, DbError>>),
    GetPost {
        id: i64,
        reply: Reply<Result<Option<Vec<u8>>, DbError>>,
    },
    Create {
        user_id: i64,
        body: String,
        author: String,
        reply: Reply<Result<Vec<u8>, DbError>>,
    },
    Like {
        user_id: i64,
        post_id: i64,
        reply: Reply<Result<LikeOutcome, DbError>>,
    },
    Ping(Reply<Result<(), String>>),
}

#[derive(Clone)]
pub struct Handle {
    tx: mpsc::Sender<Command>,
}

impl Handle {
    pub async fn feed(&self) -> Result<Vec<u8>, DbError> {
        let (reply, result) = oneshot::channel();
        self.tx
            .send(Command::Feed(reply))
            .await
            .map_err(|_| DbError::Unavailable)?;
        result.await.map_err(|_| DbError::Unavailable)?
    }

    pub async fn get_post(&self, id: i64) -> Result<Option<Vec<u8>>, DbError> {
        let (reply, result) = oneshot::channel();
        self.tx
            .send(Command::GetPost { id, reply })
            .await
            .map_err(|_| DbError::Unavailable)?;
        result.await.map_err(|_| DbError::Unavailable)?
    }

    pub async fn create(
        &self,
        user_id: i64,
        body: String,
        author: String,
    ) -> Result<Vec<u8>, DbError> {
        let (reply, result) = oneshot::channel();
        self.tx
            .send(Command::Create {
                user_id,
                body,
                author,
                reply,
            })
            .await
            .map_err(|_| DbError::Unavailable)?;
        result.await.map_err(|_| DbError::Unavailable)?
    }

    pub async fn like(&self, user_id: i64, post_id: i64) -> Result<LikeOutcome, DbError> {
        let (reply, result) = oneshot::channel();
        self.tx
            .send(Command::Like {
                user_id,
                post_id,
                reply,
            })
            .await
            .map_err(|_| DbError::Unavailable)?;
        result.await.map_err(|_| DbError::Unavailable)?
    }

    pub async fn ping(&self) -> Result<(), String> {
        let (reply, result) = oneshot::channel();
        if self.tx.send(Command::Ping(reply)).await.is_err() {
            return Err("database actor unavailable".to_owned());
        }
        result
            .await
            .unwrap_or_else(|_| Err("database actor unavailable".to_owned()))
    }
}

/// Opens and configures the database, then starts the actor and checkpointer.
pub fn start(path: &str) -> Result<Handle, rusqlite::Error> {
    let conn = Connection::open(path)?;
    conn.execute_batch(
        "PRAGMA journal_mode = WAL;
         PRAGMA synchronous = NORMAL;
         PRAGMA foreign_keys = ON;
         PRAGMA busy_timeout = 5000;
         PRAGMA cache_size = -65536;
         PRAGMA mmap_size = 0;
         PRAGMA temp_store = MEMORY;
         PRAGMA journal_size_limit = 67108864;
         PRAGMA wal_autocheckpoint = 10000;",
    )?;

    let (tx, rx) = mpsc::channel(COMMAND_QUEUE);
    thread::Builder::new()
        .name("sqlite-actor".to_owned())
        .spawn(move || run_actor(conn, rx))
        .expect("failed to spawn sqlite actor");

    spawn_checkpointer(path.to_owned());

    Ok(Handle { tx })
}

fn spawn_checkpointer(path: String) {
    let _ = thread::Builder::new()
        .name("wal-checkpoint".to_owned())
        .spawn(move || {
            // Keep checkpoint I/O from competing with request processing.
            unsafe {
                libc::setpriority(libc::PRIO_PROCESS, 0, 19);
            }

            let Ok(conn) = Connection::open(&path) else {
                return;
            };
            if conn.execute_batch("PRAGMA busy_timeout = 100;").is_err() {
                return;
            }

            loop {
                thread::sleep(CHECKPOINT_INTERVAL);
                let _ = conn.execute_batch("PRAGMA wal_checkpoint(PASSIVE);");
            }
        });
}

fn run_actor(conn: Connection, mut rx: mpsc::Receiver<Command>) {
    let mut batch = Vec::with_capacity(BATCH_MAX);
    loop {
        batch.clear();
        if rx.blocking_recv_many(&mut batch, BATCH_MAX) == 0 {
            return;
        }
        process_batch(&conn, &mut batch);
    }
}

fn process_batch(conn: &Connection, batch: &mut Vec<Command>) {
    let mut writes = Vec::new();

    for command in batch.drain(..) {
        match command {
            Command::Feed(reply) => {
                let _ = reply.send(feed(conn));
            }
            Command::GetPost { id, reply } => {
                let _ = reply.send(get_post(conn, id));
            }
            Command::Ping(reply) => {
                let _ = reply.send(ping(conn));
            }
            Command::Create { .. } | Command::Like { .. } => writes.push(command),
        }
    }

    if !writes.is_empty() {
        process_writes(conn, writes);
    }
}

fn feed(conn: &Connection) -> Result<Vec<u8>, DbError> {
    (|| -> rusqlite::Result<Vec<u8>> {
        let mut stmt = conn.prepare_cached(FEED_SQL)?;
        let mut rows = stmt.query([])?;

        let mut out = Writer::with_capacity(4096);
        out.raw("{\"posts\":[");
        let mut first = true;
        while let Some(row) = rows.next()? {
            if !first {
                out.byte(b',');
            }
            first = false;
            write_post_row(&mut out, row)?;
        }
        out.raw("]}");
        Ok(out.into_bytes())
    })()
    .map_err(|_| DbError::Unavailable)
}

fn get_post(conn: &Connection, id: i64) -> Result<Option<Vec<u8>>, DbError> {
    (|| -> rusqlite::Result<Option<Vec<u8>>> {
        let mut stmt = conn.prepare_cached(GET_POST_SQL)?;
        let mut rows = stmt.query(params![id])?;

        let Some(row) = rows.next()? else {
            return Ok(None);
        };

        let mut out = Writer::with_capacity(512);
        out.raw("{\"post\":");
        write_post_row(&mut out, row)?;
        out.byte(b'}');
        Ok(Some(out.into_bytes()))
    })()
    .map_err(|_| DbError::Unavailable)
}

fn ping(conn: &Connection) -> Result<(), String> {
    conn.query_row("SELECT 1", [], |_| Ok(()))
        .map_err(|e| e.to_string())
}

fn post_exists(conn: &Connection, id: i64) -> rusqlite::Result<bool> {
    conn.query_row(
        "SELECT EXISTS(SELECT 1 FROM posts WHERE id = ?1)",
        params![id],
        |row| row.get(0),
    )
}

fn write_post_row(out: &mut Writer, row: &rusqlite::Row<'_>) -> rusqlite::Result<()> {
    let id: i64 = row.get(0)?;
    let body = row.get_ref(1)?;
    let created_at = row.get_ref(2)?;
    let author = row.get_ref(3)?;
    let like_count: i64 = row.get(4)?;

    json::post(
        out,
        id,
        text(&body),
        text(&created_at),
        text(&author),
        like_count,
    );
    Ok(())
}

fn text<'a>(value: &'a ValueRef<'_>) -> &'a [u8] {
    match value {
        ValueRef::Text(bytes) => bytes,
        _ => b"",
    }
}

fn process_writes(conn: &Connection, writes: Vec<Command>) {
    let mut create_stmt = match conn.prepare_cached(CREATE_POST_SQL) {
        Ok(stmt) => stmt,
        Err(_) => {
            fail_writes(writes);
            return;
        }
    };
    let mut like_stmt = match conn.prepare_cached(LIKE_SQL) {
        Ok(stmt) => stmt,
        Err(_) => {
            fail_writes(writes);
            return;
        }
    };

    if conn.execute_batch("BEGIN IMMEDIATE").is_err() {
        fail_writes(writes);
        return;
    }

    enum Done {
        Create(Reply<Result<Vec<u8>, DbError>>, Result<Vec<u8>, DbError>),
        Like(
            Reply<Result<LikeOutcome, DbError>>,
            Result<LikeOutcome, DbError>,
        ),
    }

    let mut done = Vec::with_capacity(writes.len());
    let mut fatal = false;

    for command in writes {
        match command {
            Command::Create {
                user_id,
                body,
                author,
                reply,
            } => {
                let result = match insert_post(&mut create_stmt, user_id, &body, &author) {
                    Ok(body) => Ok(body),
                    Err(error) => {
                        if is_fatal(&error) {
                            fatal = true;
                        }
                        Err(DbError::Unavailable)
                    }
                };
                done.push(Done::Create(reply, result));
            }
            Command::Like {
                user_id,
                post_id,
                reply,
            } => {
                let result = match like_stmt.execute(params![user_id, post_id]) {
                    Ok(1) => Ok(LikeOutcome::Created(like_body(false, post_id))),
                    Ok(_) => Ok(LikeOutcome::AlreadyLiked(like_body(true, post_id))),
                    Err(error) if is_constraint_violation(&error) => {
                        // The foreign key can fail on the post or on the
                        // token's user id. Only the former is a missing post.
                        match post_exists(conn, post_id) {
                            Ok(true) => Err(DbError::Unavailable),
                            Ok(false) => Ok(LikeOutcome::PostNotFound),
                            Err(_) => {
                                fatal = true;
                                Err(DbError::Unavailable)
                            }
                        }
                    }
                    Err(error) => {
                        if is_fatal(&error) {
                            fatal = true;
                        }
                        Err(DbError::Unavailable)
                    }
                };
                done.push(Done::Like(reply, result));
            }
            _ => unreachable!("only writes are passed to process_writes"),
        }
    }

    let committed = if !fatal && conn.execute_batch("COMMIT").is_ok() {
        true
    } else {
        let _ = conn.execute_batch("ROLLBACK");
        false
    };

    for entry in done {
        match entry {
            Done::Create(reply, result) => {
                let _ = reply.send(if committed {
                    result
                } else {
                    Err(DbError::Unavailable)
                });
            }
            Done::Like(reply, result) => {
                // "post not found" needed no write, so it stays valid even if
                // the batch's other writes could not be committed.
                let result = if committed || matches!(result, Ok(LikeOutcome::PostNotFound)) {
                    result
                } else {
                    Err(DbError::Unavailable)
                };
                let _ = reply.send(result);
            }
        }
    }
}

fn fail_writes(writes: Vec<Command>) {
    for command in writes {
        match command {
            Command::Create { reply, .. } => {
                let _ = reply.send(Err(DbError::Unavailable));
            }
            Command::Like { reply, .. } => {
                let _ = reply.send(Err(DbError::Unavailable));
            }
            _ => {}
        }
    }
}

fn insert_post(
    stmt: &mut CachedStatement<'_>,
    user_id: i64,
    body: &str,
    author: &str,
) -> rusqlite::Result<Vec<u8>> {
    let mut out = Writer::with_capacity(body.len() + author.len() + 192);

    stmt.query_row(params![user_id, body], |row| {
        let id: i64 = row.get(0)?;
        let created_at = row.get_ref(1)?;

        out.raw("{\"post\":");
        json::post(
            &mut out,
            id,
            body.as_bytes(),
            text(&created_at),
            author.as_bytes(),
            0,
        );
        out.byte(b'}');
        Ok(())
    })?;

    Ok(out.into_bytes())
}

fn like_body(already_liked: bool, post_id: i64) -> Vec<u8> {
    let mut out = Writer::with_capacity(64);
    out.raw("{\"liked\":true,\"already_liked\":");
    out.bool(already_liked);
    out.raw(",\"post_id\":");
    out.int(post_id);
    out.byte(b'}');
    out.into_bytes()
}

fn is_constraint_violation(error: &rusqlite::Error) -> bool {
    matches!(
        error,
        rusqlite::Error::SqliteFailure(failure, _)
            if failure.code == ErrorCode::ConstraintViolation
    )
}

fn is_busy(error: &rusqlite::Error) -> bool {
    matches!(
        error,
        rusqlite::Error::SqliteFailure(failure, _)
            if matches!(
                failure.code,
                ErrorCode::DatabaseBusy | ErrorCode::DatabaseLocked
            )
    )
}

/// Constraint failures abort only their statement, so the rest of the batch
/// can still commit. Anything else leaves the transaction's state uncertain.
fn is_fatal(error: &rusqlite::Error) -> bool {
    !is_constraint_violation(error) && !is_busy(error)
}
