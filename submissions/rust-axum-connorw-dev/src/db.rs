//! SQLite. Reads run on the calling thread with a pooled read-only connection. Writes are queued
//! to one writer thread that commits them in groups with `synchronous=FULL`, and each caller gets
//! its result only after the transaction holding its write has committed.

use std::{
    error::Error,
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Mutex, PoisonError},
    thread,
    time::Duration,
};

use rusqlite::{Connection, OpenFlags, OptionalExtension, Row, ffi};
use serde::Serialize;
use tokio::sync::{mpsc, oneshot};

/// How long the writer waits for more writes to join a transaction. Every COMMIT costs an fsync
/// and makes the read connections drop their page caches, so batching a few milliseconds of
/// writes cuts CPU per request by about 15% under load, for 5 ms more latency on each write.
const COMMIT_WINDOW: Duration = Duration::from_millis(5);
/// Enough to keep up with ~500 writes/s even if an fsync stalls for two seconds.
const MAX_BATCH: usize = 1024;

/// A post as the API returns it. The feed serializes `Post<&str>`, whose strings borrow SQLite's
/// row buffer; everywhere else a `Post` owns its strings.
#[derive(Debug, Serialize)]
pub struct Post<S = String> {
    pub id: i64,
    pub body: S,
    pub created_at: S,
    pub author: S,
    pub like_count: i64,
}

pub enum Like {
    Added,
    AlreadyLiked,
    NoSuchPost,
}

/// The 20 newest posts, serialized as the body of `GET /feed`. Each row goes to JSON straight from
/// SQLite's buffers; copying rows into `Post`s with owned strings first cost 11% of a feed's CPU.
pub fn feed(db: &Connection) -> rusqlite::Result<Vec<u8>> {
    let mut query = db.prepare_cached(
        "SELECT p.id, p.body, p.created_at, u.username,
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
           FROM posts p JOIN users u ON u.id = p.user_id
          ORDER BY p.created_at DESC, p.id DESC
          LIMIT 20",
    )?;
    let mut rows = query.query([])?;
    let mut json = Vec::with_capacity(8192); // a feed is about 5 KB
    json.extend_from_slice(br#"{"posts":["#);
    while let Some(row) = rows.next()? {
        if json.ends_with(b"}") {
            json.push(b',');
        }
        let post = Post {
            id: row.get(0)?,
            body: row.get_ref(1)?.as_str()?,
            created_at: row.get_ref(2)?.as_str()?,
            author: row.get_ref(3)?.as_str()?,
            like_count: row.get(4)?,
        };
        serde_json::to_writer(&mut json, &post).expect("serializing into a Vec cannot fail");
    }
    json.extend_from_slice(b"]}");
    Ok(json)
}

pub fn post(db: &Connection, id: i64) -> rusqlite::Result<Option<Post>> {
    db.prepare_cached(
        "SELECT p.id, p.body, p.created_at, u.username,
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
           FROM posts p JOIN users u ON u.id = p.user_id
          WHERE p.id = ?1",
    )?
    .query_row([id], post_from_row)
    .optional()
}

fn post_from_row(row: &Row) -> rusqlite::Result<Post> {
    Ok(Post {
        id: row.get(0)?,
        body: row.get(1)?,
        created_at: row.get(2)?,
        author: row.get(3)?,
        like_count: row.get(4)?,
    })
}

pub fn create_post(db: &Connection, user_id: i64, author: String, body: String) -> rusqlite::Result<Post> {
    let (id, created_at) = db
        .prepare_cached("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")?
        .query_row((user_id, &body), |row| Ok((row.get(0)?, row.get(1)?)))?;
    Ok(Post { id, body, created_at, author, like_count: 0 })
}

pub fn like(db: &Connection, user_id: i64, post_id: i64) -> rusqlite::Result<Like> {
    // Inserts only if the post exists; when nothing was inserted, a lookup tells the two cases apart.
    let added = db
        .prepare_cached(
            "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2
              WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING",
        )?
        .execute((user_id, post_id))?;
    if added == 1 {
        return Ok(Like::Added);
    }
    let exists: bool = db
        .prepare_cached("SELECT EXISTS (SELECT 1 FROM posts WHERE id = ?1)")?
        .query_row([post_id], |row| row.get(0))?;
    Ok(if exists { Like::AlreadyLiked } else { Like::NoSuchPost })
}

/// The database: a pool of read connections, and a queue to the writer thread.
pub struct Db {
    path: String,
    readers: Mutex<Vec<Connection>>,
    writes: mpsc::Sender<Job>,
}

/// A write, run on the writer thread. It returns what to do once its transaction has committed
/// (`true`) or rolled back (`false`).
type Job = Box<dyn FnOnce(&Connection) -> Box<dyn FnOnce(bool) + Send> + Send>;

impl Db {
    /// Opens the database and starts the writer thread.
    pub fn open(path: &str) -> Result<Db, Box<dyn Error>> {
        let writer = connect(path, true)?;
        let reader = connect(path, false)?; // fail at startup, not on the first request
        let (writes, queue) = mpsc::channel(4096); // when full, requests wait instead of piling up
        thread::Builder::new().name("sqlite-writer".into()).spawn(move || group_commit(writer, queue))?;
        Ok(Db { path: path.to_owned(), readers: Mutex::new(vec![reader]), writes })
    }

    /// Runs `query` on the calling thread. Reads take microseconds, so a hop to another thread
    /// would cost more than the query. One connection per thread that reads concurrently.
    pub fn read<T>(&self, query: impl FnOnce(&Connection) -> rusqlite::Result<T>) -> rusqlite::Result<T> {
        let pooled = self.readers.lock().unwrap_or_else(PoisonError::into_inner).pop();
        let db = match pooled {
            Some(db) => db,
            None => connect(&self.path, false)?,
        };
        let result = query(&db);
        self.readers.lock().unwrap_or_else(PoisonError::into_inner).push(db);
        result
    }

    /// For the health check: a read succeeds, and the writer thread is still there to take writes.
    pub fn check(&self) -> rusqlite::Result<()> {
        if self.writes.is_closed() {
            return Err(aborted("writer thread has stopped"));
        }
        self.read(|db| db.query_row("SELECT 1", [], |_| Ok(())))
    }

    /// Queues `change` for the writer thread and resolves once the transaction holding it has
    /// committed, so the change is on disk before the caller answers anyone.
    pub async fn write<T: Send + 'static>(
        &self,
        change: impl FnOnce(&Connection) -> rusqlite::Result<T> + Send + 'static,
    ) -> rusqlite::Result<T> {
        let (reply, response) = oneshot::channel();
        let job: Job = Box::new(move |db| {
            let result = change(db);
            Box::new(move |committed| {
                let _ = reply.send(if committed { result } else { Err(aborted("transaction rolled back")) });
            })
        });
        self.writes.send(job).await.map_err(|_| aborted("writer thread has stopped"))?;
        response.await.map_err(|_| aborted("write abandoned"))?
    }
}

/// Group commit: each pass takes every write that queued up while the previous COMMIT ran, plus
/// any that arrive within COMMIT_WINDOW, and runs them in one transaction.
fn group_commit(db: Connection, mut queue: mpsc::Receiver<Job>) {
    let mut batch = Vec::with_capacity(MAX_BATCH);
    while queue.blocking_recv_many(&mut batch, MAX_BATCH) > 0 {
        if batch.len() < MAX_BATCH {
            thread::sleep(COMMIT_WINDOW);
        }
        while batch.len() < MAX_BATCH
            && let Ok(job) = queue.try_recv()
        {
            batch.push(job);
        }
        if let Err(e) = db.execute_batch("BEGIN IMMEDIATE") {
            eprintln!("sqlite: begin: {e}");
            batch.clear(); // a dropped job drops its reply channel, so its caller gets an error
            continue;
        }
        // A failed statement usually rolls back only itself, and a panicking job loses only its own
        // reply. But disk-full, I/O, out-of-memory and busy errors can roll back the whole
        // transaction, and a job run after that would commit on its own while its caller got an
        // error. So stop there: the jobs not yet run are dropped, and their callers get errors.
        let mut finish = Vec::with_capacity(batch.len());
        for job in batch.drain(..) {
            if db.is_autocommit() {
                break;
            }
            finish.extend(catch_unwind(AssertUnwindSafe(|| job(&db))).ok());
        }
        let committed =
            !db.is_autocommit() && db.execute_batch("COMMIT").inspect_err(|e| eprintln!("sqlite: commit: {e}")).is_ok();
        if !db.is_autocommit() {
            let _ = db.execute_batch("ROLLBACK");
        }
        finish.into_iter().for_each(|finish| finish(committed));
    }
}

fn aborted(why: &str) -> rusqlite::Error {
    rusqlite::Error::SqliteFailure(ffi::Error::new(ffi::SQLITE_ABORT), Some(why.to_owned()))
}

fn connect(path: &str, writer: bool) -> rusqlite::Result<Connection> {
    let db = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX)?;
    db.execute_batch("PRAGMA busy_timeout = 5000; PRAGMA mmap_size = 1073741824; PRAGMA cache_size = -4096;")?;
    db.execute_batch(if writer {
        // FULL: COMMIT fsyncs the WAL, so a write the API has acknowledged survives power loss.
        "PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL; PRAGMA foreign_keys = ON;"
    } else {
        "PRAGMA query_only = ON;"
    })?;
    Ok(db)
}

#[cfg(test)]
pub(crate) mod tests {
    //! The writer's failure paths, against a small temporary database.
    use super::*;
    use std::{
        path::PathBuf,
        sync::atomic::{AtomicUsize, Ordering},
        time::Instant,
    };

    /// schema.sql, two users, two posts and a like.
    const SCHEMA_AND_ROWS: &str = "
        PRAGMA journal_mode = WAL;
        CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
            created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
        CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
            body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
            created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
        CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id),
            post_id INTEGER NOT NULL REFERENCES posts(id),
            created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
            PRIMARY KEY (user_id, post_id));
        CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
        CREATE INDEX posts_user_id_idx ON posts (user_id);
        CREATE INDEX likes_post_id_idx ON likes (post_id);
        INSERT INTO users (id, username) VALUES (1, 'ann'), (2, 'bob');
        INSERT INTO posts (id, user_id, body, created_at) VALUES
            (1, 1, 'first', '2026-01-01T00:00:00.000Z'),
            (2, 2, 'say \"hi\" \\ bye', '2026-01-02T00:00:00.000Z');
        INSERT INTO likes (user_id, post_id) VALUES (2, 1);";

    /// A fresh database file, deleted along with its WAL when this drops.
    pub struct TempDb(PathBuf);

    impl TempDb {
        pub fn new() -> TempDb {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let name = format!("feed-test-{}-{}.db", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed));
            let file = TempDb(std::env::temp_dir().join(name));
            Connection::open(&file.0).unwrap().execute_batch(SCHEMA_AND_ROWS).unwrap();
            file
        }

        pub fn path(&self) -> &str {
            self.0.to_str().unwrap()
        }

        /// (posts, likes), read through a connection of its own.
        fn rows(&self) -> (i64, i64) {
            let db = Connection::open(&self.0).unwrap();
            let count = |table: &str| db.query_row(&format!("SELECT count(*) FROM {table}"), [], |row| row.get(0));
            (count("posts").unwrap(), count("likes").unwrap())
        }
    }

    impl Drop for TempDb {
        fn drop(&mut self) {
            for suffix in ["", "-wal", "-shm"] {
                let _ = std::fs::remove_file(format!("{}{suffix}", self.0.display()));
            }
        }
    }

    fn new_post<'a>(db: &'a Db, body: &'static str) -> impl Future<Output = rusqlite::Result<Post>> + 'a {
        db.write(move |db| create_post(db, 1, "ann".to_owned(), body.to_owned()))
    }

    /// Runs `writes` while the writer is busy with an earlier batch, so they queue up and share the
    /// next transaction.
    async fn in_one_batch<F: Future>(db: &Db, writes: F) -> F::Output {
        let (started, busy) = oneshot::channel();
        let blocker = db.write(move |_| {
            let _ = started.send(());
            thread::sleep(Duration::from_millis(100));
            Ok(())
        });
        let (_, output) = tokio::join!(blocker, async {
            busy.await.unwrap();
            writes.await
        });
        output
    }

    #[tokio::test]
    async fn a_failed_statement_rolls_back_alone() {
        let file = TempDb::new();
        let db = Db::open(file.path()).unwrap();
        // A like for a post that doesn't exist breaks a foreign key, so that statement fails.
        let orphan = db.write(|db| db.execute("INSERT INTO likes (user_id, post_id) VALUES (1, 999)", []));
        let (before, like, after) =
            in_one_batch(&db, async { tokio::join!(new_post(&db, "before"), orphan, new_post(&db, "after")) }).await;
        assert!(before.is_ok() && like.is_err() && after.is_ok(), "{like:?}");
        assert_eq!(file.rows(), (4, 1));
    }

    #[tokio::test]
    async fn a_failed_commit_fails_every_write_in_it() {
        let file = TempDb::new();
        let db = Db::open(file.path()).unwrap();
        // With the foreign key check deferred, the orphan like itself succeeds and the COMMIT fails.
        let orphan = db.write(|db| {
            db.execute_batch("PRAGMA defer_foreign_keys = ON")?;
            db.execute("INSERT INTO likes (user_id, post_id) VALUES (1, 999)", [])
        });
        let (like, post) = in_one_batch(&db, async { tokio::join!(orphan, new_post(&db, "lost")) }).await;
        assert!(like.is_err() && post.is_err(), "{like:?} {post:?}");
        assert_eq!(file.rows(), (2, 1));
        assert!(new_post(&db, "next").await.is_ok()); // the writer carries on
    }

    #[tokio::test]
    async fn nothing_runs_after_sqlite_rolls_back_the_whole_batch() {
        let file = TempDb::new();
        let db = Db::open(file.path()).unwrap();
        // Disk-full, I/O, out-of-memory and busy errors can roll back the whole transaction; this stands in.
        let rolls_back = db.write(|db| db.execute_batch("ROLLBACK"));
        let (rolled_back, post) = in_one_batch(&db, async { tokio::join!(rolls_back, new_post(&db, "after")) }).await;
        assert!(rolled_back.is_err() && post.is_err());
        assert_eq!(file.rows(), (2, 1)); // the post was never written, so retrying its 500 can't duplicate it
    }

    #[tokio::test]
    async fn a_panicking_write_loses_only_its_own_reply() {
        let file = TempDb::new();
        let db = Db::open(file.path()).unwrap();
        let panics = db.write(|_| -> rusqlite::Result<()> { panic!("a bug in one handler") });
        let (panicked, post) = in_one_batch(&db, async { tokio::join!(panics, new_post(&db, "kept")) }).await;
        assert!(panicked.is_err() && post.is_ok());
        assert_eq!(file.rows(), (3, 1));
    }

    #[tokio::test]
    async fn writes_fail_instead_of_hanging_while_another_process_holds_the_lock() {
        let file = TempDb::new();
        let db = Db::open(file.path()).unwrap();
        let other = Connection::open(file.path()).unwrap();
        other.execute_batch("BEGIN IMMEDIATE").unwrap();
        let started = Instant::now();
        assert!(new_post(&db, "blocked").await.is_err()); // after busy_timeout
        assert!(started.elapsed() >= Duration::from_secs(4));
        other.execute_batch("ROLLBACK").unwrap();
        assert!(new_post(&db, "unblocked").await.is_ok());
    }
}
