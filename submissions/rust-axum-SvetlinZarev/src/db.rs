use std::cell::RefCell;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use rusqlite::{Connection, OpenFlags};
use tokio::sync::{mpsc, oneshot};

pub const SQL_FEED: &str = "SELECT p.id,p.body,p.created_at,u.username,\
    (SELECT count(*) FROM likes l WHERE l.post_id=p.id) \
    FROM posts p JOIN users u ON u.id=p.user_id \
    ORDER BY p.created_at DESC, p.id DESC LIMIT 20";

pub const SQL_POST_BY_ID: &str = "SELECT p.id,p.body,p.created_at,u.username,\
    (SELECT count(*) FROM likes l WHERE l.post_id=p.id) \
    FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?";

const SQL_INSERT_POST: &str =
    "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at";

const SQL_INSERT_LIKE: &str = "INSERT INTO likes (user_id, post_id) \
    SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id=?2) \
    ON CONFLICT (user_id, post_id) DO NOTHING";

const SQL_POST_EXISTS: &str = "SELECT 1 FROM posts WHERE id=?";

const MAX_BATCH: usize = 32;

const CHECKPOINT_FRAMES: i64 = 1000;

const CHECKPOINT_INTERVAL: Duration = Duration::from_millis(250);

static DB_PATH: OnceLock<String> = OnceLock::new();

thread_local! {
    static READ_CONN: RefCell<Option<Connection>> = const { RefCell::new(None) };
}

pub enum Write {
    CreatePost {
        user_id: i64,
        body: String,
        reply: oneshot::Sender<rusqlite::Result<(i64, String)>>,
    },

    Like {
        user_id: i64,
        post_id: i64,
        reply: oneshot::Sender<rusqlite::Result<LikeOutcome>>,
    },
}

pub enum LikeOutcome {
    Created,
    Existing,
    Missing,
}

enum Outcome {
    CreatePost((i64, String)),
    Like(LikeOutcome),
}

struct WriteStmts<'c> {
    insert_post: rusqlite::Statement<'c>,
    insert_like: rusqlite::Statement<'c>,
    post_exists: rusqlite::Statement<'c>,
}

impl<'c> WriteStmts<'c> {
    fn new(conn: &'c Connection) -> Self {
        WriteStmts {
            insert_post: conn.prepare(SQL_INSERT_POST).unwrap(),
            insert_like: conn.prepare(SQL_INSERT_LIKE).unwrap(),
            post_exists: conn.prepare(SQL_POST_EXISTS).unwrap(),
        }
    }

    fn create_post(&mut self, user_id: i64, body: &str) -> rusqlite::Result<(i64, String)> {
        self.insert_post
            .query_row((user_id, body), |row| Ok((row.get(0)?, row.get(1)?)))
    }

    fn like(&mut self, user_id: i64, post_id: i64) -> rusqlite::Result<LikeOutcome> {
        let changed = self.insert_like.execute((user_id, post_id))?;
        if changed == 1 {
            return Ok(LikeOutcome::Created);
        }
        let exists = self.post_exists.exists([post_id])?;
        Ok(if exists {
            LikeOutcome::Existing
        } else {
            LikeOutcome::Missing
        })
    }
}

pub fn init_read_path(path: &str) {
    let _ = DB_PATH.set(path.to_string());
}

pub fn with_read<T>(f: impl FnOnce(&Connection) -> T) -> T {
    READ_CONN.with(|cell| {
        let mut slot = cell.borrow_mut();
        let conn = slot.get_or_insert_with(|| {
            open_conn(DB_PATH.get().expect("read path not initialized"), true)
        });
        f(conn)
    })
}

pub fn open_conn(path: &str, read_only: bool) -> Connection {
    let flags = if read_only {
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX
    } else {
        OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX
    };
    let conn = Connection::open_with_flags(path, flags).expect("open sqlite");
    conn.pragma_update(None, "journal_mode", "WAL").unwrap();
    conn.pragma_update(None, "synchronous", "NORMAL").unwrap();
    conn.pragma_update(None, "busy_timeout", 5000).unwrap();
    conn.pragma_update(None, "mmap_size", 1_073_741_824i64)
        .unwrap();
    conn.pragma_update(None, "cache_size", -65536).unwrap();
    conn.pragma_update(None, "temp_store", "MEMORY").unwrap();
    if !read_only {
        conn.pragma_update(None, "wal_autocheckpoint", 0).unwrap();
    }
    conn
}

pub fn writer_loop(path: String, mut rx: mpsc::Receiver<Write>) {
    let conn = open_conn(&path, false);
    let mut stmts = WriteStmts::new(&conn);
    let mut batch: Vec<Write> = Vec::with_capacity(MAX_BATCH);
    let mut last_checkpoint = Instant::now();
    while let Some(first) = rx.blocking_recv() {
        batch.push(first);
        while batch.len() < MAX_BATCH {
            match rx.try_recv() {
                Ok(req) => batch.push(req),
                Err(_) => break,
            }
        }
        commit_batch(&conn, &mut stmts, &mut batch);
        if rx.is_empty() || last_checkpoint.elapsed() >= CHECKPOINT_INTERVAL {
            maybe_checkpoint(&conn);
            last_checkpoint = Instant::now();
        }
    }
}

fn maybe_checkpoint(conn: &Connection) {
    let log_frames = conn.query_row("PRAGMA wal_checkpoint(PASSIVE)", [], |row| row.get::<_, i64>(1));
    if let Ok(frames) = log_frames
        && frames >= CHECKPOINT_FRAMES
    {
        let _ = conn.query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |_| Ok(()));
    }
}

fn commit_batch(conn: &Connection, stmts: &mut WriteStmts, batch: &mut Vec<Write>) {
    if batch.len() == 1 {
        run_single(stmts, batch.pop().unwrap());
        return;
    }

    let mut pending = std::mem::take(batch);

    if conn.execute_batch("BEGIN").is_err() {
        return retry_individually(conn, stmts, pending);
    }

    let mut results = Vec::with_capacity(pending.len());
    for req in &pending {
        let outcome = match req {
            Write::CreatePost { user_id, body, .. } => {
                stmts.create_post(*user_id, body).map(Outcome::CreatePost)
            }
            Write::Like {
                user_id, post_id, ..
            } => stmts.like(*user_id, *post_id).map(Outcome::Like),
        };
        match outcome {
            Ok(o) => results.push(o),
            Err(_) => {
                let _ = conn.execute_batch("ROLLBACK");
                return retry_individually(conn, stmts, pending);
            }
        }
    }

    if conn.execute_batch("COMMIT").is_err() {
        let _ = conn.execute_batch("ROLLBACK");
        return retry_individually(conn, stmts, pending);
    }

    for (req, outcome) in pending.drain(..).zip(results) {
        match (req, outcome) {
            (Write::CreatePost { reply, .. }, Outcome::CreatePost(r)) => {
                let _ = reply.send(Ok(r));
            }
            (Write::Like { reply, .. }, Outcome::Like(r)) => {
                let _ = reply.send(Ok(r));
            }
            _ => unreachable!(),
        }
    }
}

fn run_single(stmts: &mut WriteStmts, req: Write) {
    match req {
        Write::CreatePost {
            user_id,
            body,
            reply,
        } => {
            let _ = reply.send(stmts.create_post(user_id, &body));
        }
        Write::Like {
            user_id,
            post_id,
            reply,
        } => {
            let _ = reply.send(stmts.like(user_id, post_id));
        }
    }
}

fn retry_individually(conn: &Connection, stmts: &mut WriteStmts, batch: Vec<Write>) {
    for req in batch {
        let _ = conn.execute_batch("BEGIN");
        run_single(stmts, req);
        let _ = conn.execute_batch("COMMIT");
    }
}
