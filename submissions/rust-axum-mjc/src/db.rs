use std::{
    error::Error,
    fmt,
    path::Path,
    sync::mpsc as std_mpsc,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use bytes::Bytes;
use rusqlite::{Connection, OpenFlags, PrepFlags, params};
use tokio::sync::{mpsc, oneshot};

use crate::model::Post;

const POST_QUERY: &str = "SELECT p.id, p.body, p.created_at, u.username,
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
    FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1";
const FEED_QUERY: &str = "SELECT p.id, p.body, p.created_at, u.username,
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
    FROM posts p JOIN users u ON u.id = p.user_id
    ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const CREATE_POST_QUERY: &str =
    "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at";
const POST_EXISTS_QUERY: &str = "SELECT EXISTS (SELECT 1 FROM posts WHERE id = ?1)";
const INSERT_LIKE_QUERY: &str = "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING";
pub const DEFAULT_PAGE_CACHE_KIB: u64 = 32 * 1024;
const CHECKPOINT_IDLE_INTERVAL: Duration = Duration::from_secs(1);
const REQUEST_CAPACITY: usize = 4096;
// Match the benchmark p99 ceiling; admission and execution both count toward latency.
const REQUEST_ADMISSION_TIMEOUT: Duration = Duration::from_secs(1);
const READ_BATCH_LIMIT: usize = 1024;
const WRITE_BATCH_LIMIT: usize = 1024;
const BATCH_COLLECTION_WINDOW: Duration = Duration::from_millis(1);

fn sqlite_timings_enabled() -> bool {
    std::env::var("SQLITE_TIMINGS").is_ok_and(|value| {
        matches!(
            value.to_ascii_lowercase().as_str(),
            "1" | "true" | "yes" | "on"
        )
    })
}

#[derive(Default)]
struct AdmissionTimings {
    completed: AtomicU64,
    read_timeouts: AtomicU64,
    write_timeouts: AtomicU64,
    wait_us: AtomicU64,
    max_wait_us: AtomicU64,
    last_report_ms: AtomicU64,
}

impl AdmissionTimings {
    fn record(&self, started: Instant, write: bool, timed_out: bool, available_slots: usize) {
        let wait_us = started.elapsed().as_micros() as u64;
        self.completed.fetch_add(1, Ordering::Relaxed);
        self.wait_us.fetch_add(wait_us, Ordering::Relaxed);
        self.max_wait_us.fetch_max(wait_us, Ordering::Relaxed);
        if timed_out {
            let timeouts = if write {
                &self.write_timeouts
            } else {
                &self.read_timeouts
            };
            timeouts.fetch_add(1, Ordering::Relaxed);
        }
        let now_ms = unix_ms();
        let last_report = self.last_report_ms.load(Ordering::Relaxed);
        if now_ms.saturating_sub(last_report) >= 1_000
            && self
                .last_report_ms
                .compare_exchange(last_report, now_ms, Ordering::Relaxed, Ordering::Relaxed)
                .is_ok()
        {
            self.report(false, Some(available_slots));
        }
    }

    fn report(&self, final_report: bool, available_slots: Option<usize>) {
        // Cumulative counters; producers still report when the worker is blocked.
        eprintln!(
            "SQLITE_TIMING {}",
            serde_json::json!({
                "event": "queue_admission", "end_unix_ms": unix_ms(), "final": final_report,
                "completed": self.completed.load(Ordering::Relaxed),
                "read_timeouts": self.read_timeouts.load(Ordering::Relaxed),
                "write_timeouts": self.write_timeouts.load(Ordering::Relaxed),
                "wait_us": self.wait_us.load(Ordering::Relaxed),
                "max_wait_us": self.max_wait_us.load(Ordering::Relaxed),
                "available_slots": available_slots,
            })
        );
    }
}

impl Drop for AdmissionTimings {
    fn drop(&mut self) {
        if self.completed.load(Ordering::Relaxed) != 0 {
            self.report(true, None);
        }
    }
}

fn unix_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_millis() as u64)
}

#[derive(Default, serde::Serialize)]
struct BatchTotals {
    batches: u64,
    requests: usize,
    max_requests: usize,
    collection_us: u128,
    execution_us: u128,
    max_execution_us: u128,
}

struct WorkerTimings {
    last_report: Instant,
    read: BatchTotals,
    write: BatchTotals,
}

impl WorkerTimings {
    fn new() -> Self {
        Self {
            last_report: Instant::now(),
            read: BatchTotals::default(),
            write: BatchTotals::default(),
        }
    }

    fn record(
        &mut self,
        write: bool,
        requests: usize,
        started: Instant,
        collected: Instant,
        queued: usize,
    ) {
        let collection_us = collected.duration_since(started).as_micros();
        let execution_us = collected.elapsed().as_micros();
        let totals = if write {
            &mut self.write
        } else {
            &mut self.read
        };
        totals.batches += 1;
        totals.requests += requests;
        totals.max_requests = totals.max_requests.max(requests);
        totals.collection_us += collection_us;
        totals.execution_us += execution_us;
        totals.max_execution_us = totals.max_execution_us.max(execution_us);
        if execution_us >= 10_000 {
            let end_ms = unix_ms();
            eprintln!(
                "SQLITE_TIMING {}",
                serde_json::json!({
                    "event": "slow_batch", "kind": if write { "write" } else { "read" },
                    "requests": requests, "collection_us": collection_us, "execution_us": execution_us,
                    "start_unix_ms": end_ms.saturating_sub(((collection_us + execution_us) / 1_000) as u64),
                    "end_unix_ms": end_ms, "queued_after": queued,
                })
            );
        }
        if self.last_report.elapsed() >= Duration::from_secs(1) {
            self.report(false, Some(queued));
            self.read = BatchTotals::default();
            self.write = BatchTotals::default();
            self.last_report = Instant::now();
        }
    }

    fn report(&self, final_report: bool, queued: Option<usize>) {
        eprintln!(
            "SQLITE_TIMING {}",
            serde_json::json!({
                "event": "worker_batches", "end_unix_ms": unix_ms(), "final": final_report,
                "interval_us": self.last_report.elapsed().as_micros(),
                "read": self.read, "write": self.write, "queued_after": queued,
            })
        );
    }
}

impl Drop for WorkerTimings {
    fn drop(&mut self) {
        if self.read.batches != 0 || self.write.batches != 0 {
            self.report(true, None);
        }
    }
}

fn log_slow_commit(enabled: bool, started: Option<Instant>, requests: usize) {
    let Some(started) = started.filter(|_| enabled) else {
        return;
    };
    let commit_us = started.elapsed().as_micros();
    if commit_us < 1_000 {
        return;
    }
    let end_unix_ms = unix_ms();
    eprintln!(
        "SQLITE_TIMING {}",
        serde_json::json!({
            "event": "worker_commit",
            "requests": requests,
            "start_unix_ms": end_unix_ms.saturating_sub((commit_us / 1_000) as u64),
            "end_unix_ms": end_unix_ms,
            "commit_us": commit_us,
            "timestamp_resolution_ms": 1,
            "autocheckpoint_pages": 0,
        })
    );
}

// Odd generations mean SQL work is active. An unchanged even generation over
// two samples means no batch ran during that interval.
struct WorkerActivity<'a>(&'a AtomicU64);

impl Drop for WorkerActivity<'_> {
    fn drop(&mut self) {
        self.0.fetch_add(1, Ordering::Release);
    }
}

fn checkpoint_worker(
    connection: Connection,
    stop: std_mpsc::Receiver<()>,
    activity: Arc<AtomicU64>,
    timings_enabled: bool,
) {
    let mut previous = None;
    let mut caught_up_at = None;
    loop {
        match stop.recv_timeout(CHECKPOINT_IDLE_INTERVAL) {
            Ok(()) | Err(std_mpsc::RecvTimeoutError::Disconnected) => break,
            Err(std_mpsc::RecvTimeoutError::Timeout) => {}
        }
        let generation = activity.load(Ordering::Acquire);
        let unchanged = previous.replace(generation) == Some(generation);
        if generation & 1 != 0 || !unchanged || caught_up_at == Some(generation) {
            continue;
        }

        if timings_enabled {
            eprintln!(
                "SQLITE_TIMING {}",
                serde_json::json!({
                    "event": "checkpoint_start", "kind": "idle", "mode": "PASSIVE",
                    "start_unix_ms": unix_ms(), "activity_generation": generation,
                })
            );
        }
        let started = Instant::now();
        let result = connection.query_row("PRAGMA wal_checkpoint(PASSIVE)", [], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                row.get::<_, i64>(1)?,
                row.get::<_, i64>(2)?,
            ))
        });
        let elapsed_us = started.elapsed().as_micros();
        if result
            .as_ref()
            .is_ok_and(|(busy, log, done)| *busy == 0 && log == done)
        {
            caught_up_at = Some(generation);
        }
        if timings_enabled || result.is_err() {
            let ended = unix_ms();
            let mut event = serde_json::json!({
                "event": "checkpoint", "kind": "idle", "mode": "PASSIVE",
                "start_unix_ms": ended.saturating_sub((elapsed_us / 1_000) as u64),
                "end_unix_ms": ended, "elapsed_us": elapsed_us,
                "activity_generation": generation,
                "activity_generation_after": activity.load(Ordering::Acquire),
            });
            match result {
                Ok((busy, log, done)) => {
                    event["busy"] = serde_json::json!(busy);
                    event["log_frames"] = serde_json::json!(log);
                    event["checkpointed_frames"] = serde_json::json!(done);
                }
                Err(error) => event["error"] = serde_json::json!(error.to_string()),
            }
            eprintln!("SQLITE_TIMING {event}");
        }
    }
}

struct CheckpointLifecycle {
    stop: std_mpsc::Sender<()>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl Drop for CheckpointLifecycle {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[derive(Debug)]
pub enum DbError {
    Sqlite(rusqlite::Error),
    Serialize(serde_json::Error),
    InvalidUtf8(std::str::Utf8Error),
    WorkerStart(std::io::Error),
    WorkerUnavailable,
    WorkerBusy,
    ReadBatch(String),
    WriteBatch(String),
    InvalidPath,
    InvalidMmapSize,
    InvalidPageCacheSize,
    WalUnavailable,
}

impl fmt::Display for DbError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Sqlite(error) => write!(f, "SQLite: {error}"),
            Self::Serialize(error) => write!(f, "JSON serialization: {error}"),
            Self::InvalidUtf8(error) => write!(f, "invalid post body UTF-8: {error}"),
            Self::WorkerStart(error) => write!(f, "database worker: {error}"),
            Self::WorkerUnavailable => f.write_str("database worker unavailable"),
            Self::WorkerBusy => f.write_str("database worker is at capacity"),
            Self::ReadBatch(message) => write!(f, "read batch failed: {message}"),
            Self::WriteBatch(message) => write!(f, "write batch failed: {message}"),
            Self::InvalidPath => f.write_str("database must be an existing on-disk file"),
            Self::InvalidMmapSize => {
                f.write_str("SQLite mmap size must fit in a signed 64-bit integer")
            }
            Self::InvalidPageCacheSize => f.write_str(
                "SQLite page cache must be positive, fit a signed 32-bit KiB setting, and cover 1 to 1,000,000,000 database pages",
            ),
            Self::WalUnavailable => f.write_str("database did not enable WAL mode"),
        }
    }
}

impl Error for DbError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Sqlite(error) => Some(error),
            Self::Serialize(error) => Some(error),
            Self::InvalidUtf8(error) => Some(error),
            Self::WorkerStart(error) => Some(error),
            _ => None,
        }
    }
}

impl From<rusqlite::Error> for DbError {
    fn from(error: rusqlite::Error) -> Self {
        Self::Sqlite(error)
    }
}

impl From<serde_json::Error> for DbError {
    fn from(error: serde_json::Error) -> Self {
        Self::Serialize(error)
    }
}

impl From<std::str::Utf8Error> for DbError {
    fn from(error: std::str::Utf8Error) -> Self {
        Self::InvalidUtf8(error)
    }
}

struct WorkerStatements<'connection> {
    begin_read: rusqlite::Statement<'connection>,
    begin_write: rusqlite::Statement<'connection>,
    commit_read: rusqlite::Statement<'connection>,
    rollback_read: rusqlite::Statement<'connection>,
    commit_write: rusqlite::Statement<'connection>,
    rollback_write: rusqlite::Statement<'connection>,
    savepoint: rusqlite::Statement<'connection>,
    rollback_savepoint: rusqlite::Statement<'connection>,
    release_savepoint: rusqlite::Statement<'connection>,
    health: rusqlite::Statement<'connection>,
    feed: FeedStatements<'connection>,
    post: rusqlite::Statement<'connection>,
    create_post: rusqlite::Statement<'connection>,
    post_exists: rusqlite::Statement<'connection>,
    insert_like: rusqlite::Statement<'connection>,
}

impl<'connection> WorkerStatements<'connection> {
    fn prepare(connection: &'connection Connection) -> Result<Self, DbError> {
        // Persistent statements avoid exhausting SQLite's lookaside memory.
        Ok(Self {
            begin_read: connection
                .prepare_with_flags("BEGIN DEFERRED", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            begin_write: connection
                .prepare_with_flags("BEGIN IMMEDIATE", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            commit_read: connection
                .prepare_with_flags("COMMIT", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            rollback_read: connection
                .prepare_with_flags("ROLLBACK", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            commit_write: connection
                .prepare_with_flags("COMMIT", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            rollback_write: connection
                .prepare_with_flags("ROLLBACK", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            savepoint: connection.prepare_with_flags(
                "SAVEPOINT request_item",
                PrepFlags::SQLITE_PREPARE_PERSISTENT,
            )?,
            rollback_savepoint: connection.prepare_with_flags(
                "ROLLBACK TO request_item",
                PrepFlags::SQLITE_PREPARE_PERSISTENT,
            )?,
            release_savepoint: connection
                .prepare_with_flags("RELEASE request_item", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            health: connection
                .prepare_with_flags("SELECT 1", PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            feed: FeedStatements::prepare(connection)?,
            post: connection
                .prepare_with_flags(POST_QUERY, PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            create_post: connection
                .prepare_with_flags(CREATE_POST_QUERY, PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            post_exists: connection
                .prepare_with_flags(POST_EXISTS_QUERY, PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            insert_like: connection
                .prepare_with_flags(INSERT_LIKE_QUERY, PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
        })
    }
}

struct FeedStatements<'connection> {
    fallback: rusqlite::Statement<'connection>,
    ids: rusqlite::Statement<'connection>,
    likes: rusqlite::Statement<'connection>,
    rows: rusqlite::Statement<'connection>,
    scratch: Vec<u8>,
}

impl<'connection> FeedStatements<'connection> {
    fn prepare(connection: &'connection Connection) -> Result<Self, DbError> {
        Ok(Self {
            fallback: connection
                .prepare_with_flags(FEED_QUERY, PrepFlags::SQLITE_PREPARE_PERSISTENT)?,
            ids: connection.prepare_with_flags(
                "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20",
                PrepFlags::SQLITE_PREPARE_PERSISTENT,
            )?,
            likes: connection.prepare_with_flags(
                "SELECT post_id, count(*) FROM likes WHERE post_id BETWEEN ?1 AND ?2
                 GROUP BY post_id",
                PrepFlags::SQLITE_PREPARE_PERSISTENT,
            )?,
            rows: connection.prepare_with_flags(
                "SELECT p.id, p.body, p.created_at, u.username
                 FROM posts p JOIN users u ON u.id = p.user_id
                 WHERE p.id BETWEEN ?1 AND ?2",
                PrepFlags::SQLITE_PREPARE_PERSISTENT,
            )?,
            scratch: Vec::new(),
        })
    }
}

struct ReadBatchTransaction<'statement, 'connection> {
    connection: &'statement Connection,
    rollback: &'statement mut rusqlite::Statement<'connection>,
    active: bool,
}

impl<'statement, 'connection> ReadBatchTransaction<'statement, 'connection> {
    fn begin(
        connection: &'statement Connection,
        begin: &mut rusqlite::Statement<'connection>,
        rollback: &'statement mut rusqlite::Statement<'connection>,
    ) -> rusqlite::Result<Self> {
        begin.execute([])?;
        Ok(Self {
            connection,
            rollback,
            active: true,
        })
    }

    fn rollback(&mut self) -> rusqlite::Result<()> {
        if self.connection.is_autocommit() {
            self.active = false;
            return Ok(());
        }

        self.rollback.execute([])?;
        self.active = false;
        Ok(())
    }

    fn commit(mut self, commit: &mut rusqlite::Statement<'connection>) -> Result<(), String> {
        match commit.execute([]) {
            Ok(_) => {
                self.active = false;
                Ok(())
            }
            Err(error) => {
                let message = format!("commit failed: {error}");
                match self.rollback() {
                    Ok(()) => Err(message),
                    Err(rollback_error) => {
                        Err(format!("{message}; rollback failed: {rollback_error}"))
                    }
                }
            }
        }
    }
}

impl Drop for ReadBatchTransaction<'_, '_> {
    fn drop(&mut self) {
        if self.active
            && let Err(error) = self.rollback()
        {
            eprintln!("failed to roll back read batch transaction: {error}");
        }
    }
}

type Reply<T> = oneshot::Sender<Result<T, DbError>>;

enum Request {
    Health(Reply<()>),
    Feed(Reply<Bytes>),
    Post(i64, Reply<Option<Vec<u8>>>),
    CreatePost {
        user_id: i64,
        body: Bytes,
        username: String,
        reply: Reply<Vec<u8>>,
    },
    Like {
        user_id: i64,
        post_id: i64,
        reply: Reply<Option<bool>>,
    },
}

impl Request {
    fn is_read(&self) -> bool {
        matches!(self, Self::Health(_) | Self::Feed(_) | Self::Post(_, _))
    }

    fn reply_closed(&self) -> bool {
        match self {
            Self::Health(reply) => reply.is_closed(),
            Self::Feed(reply) => reply.is_closed(),
            Self::Post(_, reply) => reply.is_closed(),
            Self::CreatePost { reply, .. } => reply.is_closed(),
            Self::Like { reply, .. } => reply.is_closed(),
        }
    }
}

// A producer unparks this worker after publishing a request. The thread's
// retained wake token closes the race between observing an empty queue and
// parking; spurious wakes simply recheck the queue and the same deadline.
fn receive_until(receiver: &mut mpsc::Receiver<Request>, deadline: Instant) -> Option<Request> {
    loop {
        let remaining = deadline.checked_duration_since(Instant::now())?;
        match receiver.try_recv() {
            Ok(request) => return Some(request),
            Err(mpsc::error::TryRecvError::Disconnected) => return None,
            Err(mpsc::error::TryRecvError::Empty) => std::thread::park_timeout(remaining),
        }
    }
}

fn collect_read_batch(
    first: Request,
    receiver: &mut mpsc::Receiver<Request>,
    batch: &mut [Option<Request>; READ_BATCH_LIMIT],
) -> (usize, Option<Request>) {
    debug_assert!(first.is_read());
    let deadline = Instant::now() + BATCH_COLLECTION_WINDOW;
    batch[0] = Some(first);
    let mut len = 1;
    while len < READ_BATCH_LIMIT {
        match receive_until(receiver, deadline) {
            Some(request) if request.is_read() => {
                batch[len] = Some(request);
                len += 1;
            }
            Some(write) => return (len, Some(write)),
            None => return (len, None),
        }
    }
    (len, None)
}

enum ReadCompletion {
    Health(Reply<()>, Result<(), DbError>),
    Feed(Reply<Bytes>, Result<Bytes, DbError>),
    Post(Reply<Option<Vec<u8>>>, Result<Option<Vec<u8>>, DbError>),
}

impl ReadCompletion {
    fn fail(&mut self, message: &str) {
        match self {
            Self::Health(_, result) => *result = Err(DbError::ReadBatch(message.to_owned())),
            Self::Feed(_, result) => *result = Err(DbError::ReadBatch(message.to_owned())),
            Self::Post(_, result) => *result = Err(DbError::ReadBatch(message.to_owned())),
        }
    }

    fn send(self) {
        match self {
            Self::Health(reply, result) => {
                let _ = reply.send(result);
            }
            Self::Feed(reply, result) => {
                let _ = reply.send(result);
            }
            Self::Post(reply, result) => {
                let _ = reply.send(result);
            }
        }
    }
}

fn send_read_failure(request: Request, message: &str) {
    match request {
        Request::Health(reply) => {
            let _ = reply.send(Err(DbError::ReadBatch(message.to_owned())));
        }
        Request::Feed(reply) => {
            let _ = reply.send(Err(DbError::ReadBatch(message.to_owned())));
        }
        Request::Post(_, reply) => {
            let _ = reply.send(Err(DbError::ReadBatch(message.to_owned())));
        }
        Request::CreatePost { .. } | Request::Like { .. } => {
            unreachable!("only read requests are failed as a batch")
        }
    }
}

fn process_single_read(
    connection: &Connection,
    request: Request,
    statements: &mut WorkerStatements<'_>,
    feed_capacity: &mut usize,
    post_capacity: &mut usize,
) {
    match request {
        Request::Health(reply) => {
            let result = statements
                .health
                .query_row([], |_| Ok(()))
                .map_err(DbError::from);
            let _ = reply.send(result);
        }
        Request::Feed(reply) => {
            let result = match ReadBatchTransaction::begin(
                connection,
                &mut statements.begin_read,
                &mut statements.rollback_read,
            ) {
                Ok(transaction) => {
                    let mut result = read_feed(&mut statements.feed, feed_capacity);
                    if let Err(message) = transaction.commit(&mut statements.commit_read) {
                        result = Err(DbError::ReadBatch(message));
                    }
                    result
                }
                Err(error) => Err(DbError::ReadBatch(format!("begin failed: {error}"))),
            };
            let _ = reply.send(result);
        }
        Request::Post(id, reply) => {
            let _ = reply.send(read_post(&mut statements.post, id, post_capacity));
        }
        Request::CreatePost { .. } | Request::Like { .. } => {
            unreachable!("single-read path received a write")
        }
    }
}

fn process_read_batch(
    connection: &Connection,
    statements: &mut WorkerStatements<'_>,
    batch: &mut [Option<Request>; READ_BATCH_LIMIT],
    len: usize,
    feed_capacity: &mut usize,
    post_capacity: &mut usize,
    completed: &mut [Option<ReadCompletion>; READ_BATCH_LIMIT],
) -> bool {
    let completed = &mut completed[..len];
    debug_assert!(completed.iter().all(Option::is_none));
    let transaction = match ReadBatchTransaction::begin(
        connection,
        &mut statements.begin_read,
        &mut statements.rollback_read,
    ) {
        Ok(transaction) => transaction,
        Err(error) => {
            let message = format!("begin failed: {error}");
            for request in batch[..len].iter_mut().filter_map(Option::take) {
                if !request.reply_closed() {
                    send_read_failure(request, &message);
                }
            }
            return connection.is_autocommit();
        }
    };

    for index in 0..len {
        let Some(request) = batch[index].take() else {
            continue;
        };
        if request.reply_closed() {
            continue;
        }
        let completion = match request {
            Request::Health(reply) => {
                let result = statements
                    .health
                    .query_row([], |_| Ok(()))
                    .map_err(DbError::from);
                ReadCompletion::Health(reply, result)
            }
            Request::Feed(reply) => {
                ReadCompletion::Feed(reply, read_feed(&mut statements.feed, feed_capacity))
            }
            Request::Post(id, reply) => {
                let result = read_post(&mut statements.post, id, post_capacity);
                ReadCompletion::Post(reply, result)
            }
            Request::CreatePost { .. } | Request::Like { .. } => {
                unreachable!("read batch contains a write")
            }
        };
        completed[index] = Some(completion);
    }

    if let Err(message) = transaction.commit(&mut statements.commit_read) {
        for completion in completed.iter_mut().flatten() {
            completion.fail(&message);
        }
    }
    for completion in completed.iter_mut().filter_map(Option::take) {
        completion.send();
    }
    connection.is_autocommit()
}

enum WriteCompletion {
    Health(Reply<()>, Result<(), DbError>),
    Feed(Reply<Bytes>, Result<Bytes, DbError>),
    Post(Reply<Option<Vec<u8>>>, Result<Option<Vec<u8>>, DbError>),
    CreatePost {
        reply: Reply<Vec<u8>>,
        result: Result<Vec<u8>, DbError>,
    },
    Like {
        reply: Reply<Option<bool>>,
        result: Result<Option<bool>, DbError>,
    },
}

impl WriteCompletion {
    fn fail_success(&mut self, message: &str) {
        match self {
            Self::Health(_, result) => {
                if result.is_ok() {
                    *result = Err(DbError::WriteBatch(message.to_owned()));
                }
            }
            Self::Feed(_, result) => {
                if result.is_ok() {
                    *result = Err(DbError::WriteBatch(message.to_owned()));
                }
            }
            Self::Post(_, result) => {
                if result.is_ok() {
                    *result = Err(DbError::WriteBatch(message.to_owned()));
                }
            }
            Self::CreatePost { result, .. } => {
                if result.is_ok() {
                    *result = Err(DbError::WriteBatch(message.to_owned()));
                }
            }
            Self::Like { result, .. } => {
                if result.is_ok() {
                    *result = Err(DbError::WriteBatch(message.to_owned()));
                }
            }
        }
    }

    fn send(self) {
        match self {
            Self::Health(reply, result) => {
                let _ = reply.send(result);
            }
            Self::Feed(reply, result) => {
                let _ = reply.send(result);
            }
            Self::Post(reply, result) => {
                let _ = reply.send(result);
            }
            Self::CreatePost { reply, result } => {
                let _ = reply.send(result);
            }
            Self::Like { reply, result } => {
                let _ = reply.send(result);
            }
        }
    }
}

fn collect_write_batch(
    first: Request,
    receiver: &mut mpsc::Receiver<Request>,
    batch: &mut [Option<Request>; WRITE_BATCH_LIMIT],
) -> usize {
    debug_assert!(!first.is_read());
    let deadline = Instant::now() + BATCH_COLLECTION_WINDOW;
    batch[0] = Some(first);
    let mut len = 1;
    while len < WRITE_BATCH_LIMIT {
        match receive_until(receiver, deadline) {
            Some(request) => {
                batch[len] = Some(request);
                len += 1;
            }
            None => break,
        }
    }
    len
}

struct WriteBatchCapacities<'a> {
    feed: &'a mut usize,
    post: &'a mut usize,
    created_post: &'a mut usize,
}

fn process_write_batch(
    connection: &Connection,
    statements: &mut WorkerStatements<'_>,
    batch: &mut [Option<Request>],
    capacities: WriteBatchCapacities<'_>,
    timings_enabled: bool,
    completed: &mut [Option<WriteCompletion>; WRITE_BATCH_LIMIT],
) -> bool {
    let len = batch.len();
    assert!(len <= WRITE_BATCH_LIMIT);
    let completed = &mut completed[..len];
    debug_assert!(completed.iter().all(Option::is_none));
    if batch.iter().flatten().all(Request::reply_closed) {
        for slot in batch.iter_mut() {
            drop(slot.take());
        }
        return connection.is_autocommit();
    }

    let begin_result = statements.begin_write.execute([]);
    if let Err(error) = begin_result {
        let message = format!("begin failed: {error}");
        for (index, slot) in batch[..len].iter_mut().enumerate() {
            if let Some(request) = slot.take()
                && !request.reply_closed()
            {
                completed[index] = Some(fail_write_request(request, &message));
            }
        }
        for completion in completed.iter_mut().filter_map(Option::take) {
            completion.send();
        }
        return connection.is_autocommit();
    }

    let mut fatal: Option<String> = None;
    for index in 0..len {
        let Some(request) = batch[index].take() else {
            continue;
        };
        if request.reply_closed() {
            continue;
        }
        let is_write = !request.is_read();
        if is_write && let Err(error) = statements.savepoint.execute([]) {
            fatal = Some(format!("savepoint failed: {error}"));
            completed[index] = Some(fail_write_request(request, fatal.as_deref().unwrap()));
            break;
        }

        let result = match request {
            Request::Health(reply) => WriteCompletion::Health(
                reply,
                statements
                    .health
                    .query_row([], |_| Ok(()))
                    .map_err(DbError::from),
            ),
            Request::Feed(reply) => WriteCompletion::Feed(
                reply,
                read_feed(&mut statements.feed, &mut *capacities.feed),
            ),
            Request::Post(id, reply) => WriteCompletion::Post(
                reply,
                read_post(&mut statements.post, id, &mut *capacities.post),
            ),
            Request::CreatePost {
                user_id,
                body,
                username,
                reply,
            } => WriteCompletion::CreatePost {
                reply,
                result: insert_post(
                    &mut statements.create_post,
                    user_id,
                    body,
                    username,
                    &mut *capacities.created_post,
                ),
            },
            Request::Like {
                user_id,
                post_id,
                reply,
            } => WriteCompletion::Like {
                reply,
                result: insert_like(
                    &mut statements.post_exists,
                    &mut statements.insert_like,
                    user_id,
                    post_id,
                ),
            },
        };

        let item_failed = match &result {
            WriteCompletion::Health(_, r) => r.is_err(),
            WriteCompletion::Feed(_, r) => r.is_err(),
            WriteCompletion::Post(_, r) => r.is_err(),
            WriteCompletion::CreatePost { result: r, .. } => r.is_err(),
            WriteCompletion::Like { result: r, .. } => r.is_err(),
        };
        if is_write {
            let savepoint_result = if item_failed {
                statements
                    .rollback_savepoint
                    .execute([])
                    .and_then(|_| statements.release_savepoint.execute([]))
            } else {
                statements.release_savepoint.execute([])
            };
            if let Err(error) = savepoint_result {
                fatal = Some(format!("savepoint recovery failed: {error}"));
                completed[index] = Some(result);
                break;
            }
        }
        completed[index] = Some(result);
        if connection.is_autocommit() {
            fatal = Some("SQLite automatically rolled back the group transaction".to_owned());
            break;
        }
    }

    if fatal.is_none() {
        let commit_started = timings_enabled.then(Instant::now);
        if let Err(error) = statements.commit_write.execute([]) {
            fatal = Some(format!("commit failed: {error}"));
        }
        log_slow_commit(timings_enabled, commit_started, len);
    }
    if let Some(message) = fatal {
        if !connection.is_autocommit()
            && let Err(error) = statements.rollback_write.execute([])
        {
            let message = format!("{message}; rollback failed: {error}");
            for completion in completed.iter_mut().flatten() {
                completion.fail_success(&message);
            }
        } else {
            for completion in completed.iter_mut().flatten() {
                completion.fail_success(&message);
            }
        }
        for (index, slot) in batch[..len].iter_mut().enumerate() {
            if let Some(request) = slot.take()
                && !request.reply_closed()
            {
                completed[index] = Some(fail_write_request(request, &message));
            }
        }
    }
    for completion in completed.iter_mut().filter_map(Option::take) {
        completion.send();
    }
    connection.is_autocommit()
}

fn fail_write_request(request: Request, message: &str) -> WriteCompletion {
    let error = || DbError::WriteBatch(message.to_owned());
    match request {
        Request::Health(reply) => WriteCompletion::Health(reply, Err(error())),
        Request::Feed(reply) => WriteCompletion::Feed(reply, Err(error())),
        Request::Post(_, reply) => WriteCompletion::Post(reply, Err(error())),
        Request::CreatePost { reply, .. } => WriteCompletion::CreatePost {
            reply,
            result: Err(error()),
        },
        Request::Like { reply, .. } => WriteCompletion::Like {
            reply,
            result: Err(error()),
        },
    }
}

#[derive(Clone)]
pub struct Db {
    requests: mpsc::Sender<Request>,
    worker_thread: std::thread::Thread,
    admission_timings: Option<Arc<AdmissionTimings>>,
}

fn open_worker_connection(path: &Path) -> rusqlite::Result<Connection> {
    let flags = OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX;
    // Both connections belong to this process. unix-excl keeps their shared
    // WAL index in heap memory, avoiding filesystem faults when updating -shm.
    // The database and WAL remain on disk. Other SQLite processes must wait
    // until this process releases the database lock.
    #[cfg(unix)]
    {
        Connection::open_with_flags_and_vfs(path, flags, "unix-excl")
    }
    #[cfg(not(unix))]
    {
        Connection::open_with_flags(path, flags)
    }
}

impl Db {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, DbError> {
        Self::open_with_read_batching(path, true)
    }

    pub fn open_with_read_batching(
        path: impl AsRef<Path>,
        read_batching: bool,
    ) -> Result<Self, DbError> {
        Self::open_with_read_batching_and_mmap_size(path, read_batching, 0)
    }

    pub fn open_with_read_batching_and_mmap_size(
        path: impl AsRef<Path>,
        read_batching: bool,
        mmap_size_bytes: u64,
    ) -> Result<Self, DbError> {
        Self::open_with_page_cache(path, read_batching, mmap_size_bytes, DEFAULT_PAGE_CACHE_KIB)
    }

    pub fn open_with_page_cache(
        path: impl AsRef<Path>,
        read_batching: bool,
        mmap_size_bytes: u64,
        page_cache_kib: u64,
    ) -> Result<Self, DbError> {
        let sqlite_timings = sqlite_timings_enabled();
        let path = path.as_ref();
        if path.as_os_str().is_empty() || path == Path::new(":memory:") {
            return Err(DbError::InvalidPath);
        }
        let connection = open_worker_connection(path)?;
        connection.busy_timeout(Duration::from_secs(5))?;
        let journal_mode: String =
            connection.query_row("PRAGMA journal_mode=WAL", [], |row| row.get(0))?;
        if !journal_mode.eq_ignore_ascii_case("wal") {
            return Err(DbError::WalUnavailable);
        }
        connection.pragma_update(None, "synchronous", "NORMAL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.pragma_update(None, "wal_autocheckpoint", 0)?;
        // Discard the old tail when SQLite safely resets a checkpointed WAL.
        // New frames then extend the file instead of reading cold overwritten pages.
        connection.pragma_update(None, "journal_size_limit", 0)?;
        configure_page_cache(&connection, page_cache_kib)?;
        let effective_mmap_size = configure_mmap_size(&connection, mmap_size_bytes)?;
        let checkpoint_connection = open_worker_connection(path)?;
        checkpoint_connection.busy_timeout(Duration::ZERO)?;
        checkpoint_connection.pragma_update(None, "synchronous", "NORMAL")?;
        checkpoint_connection.pragma_update(None, "wal_autocheckpoint", 0)?;
        eprintln!(
            "SQLite mmap_size requested={mmap_size_bytes} bytes effective={effective_mmap_size} bytes page_cache_target_kib={page_cache_kib} wal_autocheckpoint=0 checkpoint_mode=background_passive_when_idle idle_sample_seconds={}",
            CHECKPOINT_IDLE_INTERVAL.as_secs(),
        );
        let worker_activity = Arc::new(AtomicU64::new(0));

        let (checkpoint_stop_tx, checkpoint_stop_rx) = std_mpsc::channel();
        let checkpoint_timings = sqlite_timings;
        let checkpoint_activity = Arc::clone(&worker_activity);
        let checkpoint_worker = std::thread::Builder::new()
            .name("sqlite-checkpoint".into())
            .spawn(move || {
                checkpoint_worker(
                    checkpoint_connection,
                    checkpoint_stop_rx,
                    checkpoint_activity,
                    checkpoint_timings,
                );
            })
            .map_err(DbError::WorkerStart)?;
        let checkpoint_lifecycle = CheckpointLifecycle {
            stop: checkpoint_stop_tx,
            worker: Some(checkpoint_worker),
        };

        // The channel bounds queued requests at 4096; the worker additionally
        // dequeues at most one mixed write batch or read batch of 1024.
        let (requests, mut receiver) = mpsc::channel::<Request>(REQUEST_CAPACITY);
        let (startup_tx, startup_rx) = std_mpsc::channel();
        let worker_sqlite_timings = sqlite_timings;
        let worker = match std::thread::Builder::new()
            .name("sqlite".into())
            .spawn(move || {
                let _checkpoint_lifecycle = checkpoint_lifecycle;
                let mut statements = match WorkerStatements::prepare(&connection) {
                    Ok(statements) => {
                        let _ = startup_tx.send(Ok(()));
                        statements
                    }
                    Err(error) => {
                        let _ = startup_tx.send(Err(error));
                        return;
                    }
                };
                // Reuse the observed capacity, while each response owns its bytes.
                let mut feed_capacity = 4096;
                // Start single-post responses at 256 bytes and retain their
                // observed capacity as a numeric hint. Typical bodies fit this
                // size; larger or escaped bodies grow the hint when encountered.
                let mut post_capacity = 256;
                let mut created_post_capacity = 256;
                let mut timings = worker_sqlite_timings.then(WorkerTimings::new);
                let mut pending = None;
                let mut read_batch: [Option<Request>; READ_BATCH_LIMIT] =
                    std::array::from_fn(|_| None);
                // Retain empty reply slots across batches. Only the current
                // batch's prefix is visited; sending takes every occupied slot.
                let mut read_completed = std::array::from_fn(|_| None);
                let mut write_completed = std::array::from_fn(|_| None);
                while let Some(request) = pending.take().or_else(|| receiver.blocking_recv()) {
                    // The handler may have been cancelled while this request
                    // waited in the queue. Avoid its SQL and release its slot.
                    if request.reply_closed() {
                        continue;
                    }
                    worker_activity.fetch_add(1, Ordering::AcqRel);
                    let _activity = WorkerActivity(&worker_activity);
                    let batch_started = timings.as_ref().map(|_| Instant::now());
                    if request.is_read() && read_batching {
                        let (batch_len, boundary) =
                            collect_read_batch(request, &mut receiver, &mut read_batch);
                        let collected = timings.as_ref().map(|_| Instant::now());
                        pending = boundary;
                        let live = read_batch[..batch_len]
                            .iter()
                            .flatten()
                            .filter(|request| !request.reply_closed())
                            .count();
                        if live > 1 {
                            let connection_usable = process_read_batch(
                                &connection,
                                &mut statements,
                                &mut read_batch,
                                batch_len,
                                &mut feed_capacity,
                                &mut post_capacity,
                                &mut read_completed,
                            );
                            if !connection_usable {
                                break;
                            }
                        } else {
                            let mut only_live = None;
                            for slot in &mut read_batch[..batch_len] {
                                if let Some(request) = slot.take()
                                    && !request.reply_closed()
                                {
                                    only_live = Some(request);
                                }
                            }
                            if let Some(request) = only_live {
                                process_single_read(
                                    &connection,
                                    request,
                                    &mut statements,
                                    &mut feed_capacity,
                                    &mut post_capacity,
                                );
                            }
                        }
                        if let (Some(timings), Some(started), Some(collected)) =
                            (&mut timings, batch_started, collected)
                        {
                            timings.record(false, batch_len, started, collected, receiver.len());
                        }
                        continue;
                    }
                    if request.is_read() {
                        process_single_read(
                            &connection,
                            request,
                            &mut statements,
                            &mut feed_capacity,
                            &mut post_capacity,
                        );
                        if let (Some(timings), Some(started)) = (&mut timings, batch_started) {
                            timings.record(false, 1, started, started, receiver.len());
                        }
                        continue;
                    }
                    let mut write_batch = std::array::from_fn(|_| None);
                    let batch_len = collect_write_batch(request, &mut receiver, &mut write_batch);
                    let collected = timings.as_ref().map(|_| Instant::now());
                    let capacities = WriteBatchCapacities {
                        feed: &mut feed_capacity,
                        post: &mut post_capacity,
                        created_post: &mut created_post_capacity,
                    };
                    let connection_usable = process_write_batch(
                        &connection,
                        &mut statements,
                        &mut write_batch[..batch_len],
                        capacities,
                        worker_sqlite_timings,
                        &mut write_completed,
                    );
                    if let (Some(timings), Some(started), Some(collected)) =
                        (&mut timings, batch_started, collected)
                    {
                        timings.record(true, batch_len, started, collected, receiver.len());
                    }
                    if !connection_usable {
                        break;
                    }
                }
                // Finalize all retained statements before closing the connection.
                drop(statements);
                drop(connection);
            }) {
            Ok(worker) => worker,
            Err(error) => return Err(DbError::WorkerStart(error)),
        };
        let startup_result = startup_rx.recv().map_err(|_| DbError::WorkerUnavailable);
        match startup_result {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                drop(requests);
                let _ = worker.join();
                return Err(error);
            }
            Err(error) => {
                drop(requests);
                let _ = worker.join();
                return Err(error);
            }
        }
        Ok(Self {
            requests,
            worker_thread: worker.thread().clone(),
            admission_timings: sqlite_timings.then(|| Arc::new(AdmissionTimings::default())),
        })
    }

    async fn reserve_request(&self, write: bool) -> Result<mpsc::Permit<'_, Request>, DbError> {
        let started = self.admission_timings.as_ref().map(|_| Instant::now());
        // Let short bursts drain without growing the channel or waiting indefinitely.
        let result = tokio::time::timeout(REQUEST_ADMISSION_TIMEOUT, self.requests.reserve()).await;
        if let (Some(timings), Some(started)) = (&self.admission_timings, started) {
            timings.record(started, write, result.is_err(), self.requests.capacity());
        }
        result
            .map_err(|_| DbError::WorkerBusy)?
            .map_err(|_| DbError::WorkerUnavailable)
    }

    async fn request<T>(&self, make: impl FnOnce(Reply<T>) -> Request) -> Result<T, DbError> {
        let permit = self.reserve_request(false).await?;
        let (reply, receiver) = oneshot::channel();
        permit.send(make(reply));
        self.worker_thread.unpark();
        receiver.await.map_err(|_| DbError::WorkerUnavailable)?
    }

    async fn write_request<T>(&self, make: impl FnOnce(Reply<T>) -> Request) -> Result<T, DbError> {
        let permit = self.reserve_request(true).await?;
        let (reply, receiver) = oneshot::channel();
        permit.send(make(reply));
        self.worker_thread.unpark();
        receiver.await.map_err(|_| DbError::WorkerUnavailable)?
    }

    pub async fn health(&self) -> Result<(), DbError> {
        self.request(Request::Health).await
    }

    pub async fn feed(&self) -> Result<Bytes, DbError> {
        self.request(Request::Feed).await
    }

    pub async fn post(&self, id: i64) -> Result<Option<Vec<u8>>, DbError> {
        self.request(|reply| Request::Post(id, reply)).await
    }

    pub async fn create_post(
        &self,
        user_id: i64,
        body: Bytes,
        username: String,
    ) -> Result<Vec<u8>, DbError> {
        self.write_request(|reply| Request::CreatePost {
            user_id,
            body,
            username,
            reply,
        })
        .await
    }

    pub async fn like(&self, user_id: i64, post_id: i64) -> Result<Option<bool>, DbError> {
        self.write_request(|reply| Request::Like {
            user_id,
            post_id,
            reply,
        })
        .await
    }
}

fn configure_page_cache(connection: &Connection, page_cache_kib: u64) -> Result<(), DbError> {
    let page_cache_kib = i32::try_from(page_cache_kib)
        .ok()
        .filter(|size| *size > 0)
        .ok_or(DbError::InvalidPageCacheSize)?;
    let page_size: u32 = connection.query_row("PRAGMA page_size", [], |row| row.get(0))?;
    let pages = page_cache_kib as u64 * 1024 / u64::from(page_size);
    if !(1..=1_000_000_000).contains(&pages) {
        return Err(DbError::InvalidPageCacheSize);
    }
    connection.pragma_update(None, "cache_size", -i64::from(page_cache_kib))?;
    Ok(())
}

fn configure_mmap_size(connection: &Connection, requested_bytes: u64) -> Result<i64, DbError> {
    let requested_bytes = i64::try_from(requested_bytes).map_err(|_| DbError::InvalidMmapSize)?;
    connection.pragma_update(None, "mmap_size", requested_bytes)?;
    Ok(connection.query_row("PRAGMA mmap_size", [], |row| row.get(0))?)
}

fn decode_post<'a>(row: &'a rusqlite::Row<'_>) -> rusqlite::Result<Post<'a>> {
    decode_post_with_likes(row, row.get(4)?)
}

fn decode_post_with_likes<'a>(
    row: &'a rusqlite::Row<'_>,
    like_count: i64,
) -> rusqlite::Result<Post<'a>> {
    Ok(Post {
        id: row.get(0)?,
        body: row.get_ref(1)?.as_str()?,
        created_at: row.get_ref(2)?.as_str()?,
        author: row.get_ref(3)?.as_str()?,
        like_count,
    })
}

fn read_feed(statements: &mut FeedStatements<'_>, capacity: &mut usize) -> Result<Bytes, DbError> {
    let ranged = read_range_feed(statements, capacity);
    // Reuse allocation capacity, never logical results between requests. Bound
    // idle scratch retention even when a database contains unusually large text.
    statements.scratch.clear();
    if statements.scratch.capacity() > 64 * 1024 {
        statements.scratch = Vec::new();
    }
    if let Some(output) = ranged? {
        return Ok(output);
    }
    let mut rows = statements.fallback.query([])?;
    let mut output = Vec::with_capacity(*capacity);
    output.extend_from_slice(b"{\"posts\":[");
    let mut first = true;
    while let Some(row) = rows.next()? {
        if !first {
            output.push(b',');
        }
        first = false;
        decode_post(row)?.write_json(&mut output)?;
    }
    drop(rows);
    output.extend_from_slice(b"]}");
    *capacity = output.capacity();
    Ok(Bytes::from(output))
}

fn read_range_feed(
    statements: &mut FeedStatements<'_>,
    capacity: &mut usize,
) -> Result<Option<Bytes>, DbError> {
    const MAX_ID_SPAN: usize = 256;
    let mut ids = [0_i64; 20];
    let mut len = 0;
    {
        let mut rows = statements.ids.query([])?;
        while let Some(row) = rows.next()? {
            ids[len] = row.get(0)?;
            len += 1;
        }
    }
    if len == 0 {
        return Ok(Some(Bytes::from_static(b"{\"posts\":[]}")));
    }
    let ids = &ids[..len];
    let minimum = *ids.iter().min().expect("nonempty feed IDs");
    let maximum = *ids.iter().max().expect("nonempty feed IDs");
    let Some(span) = maximum
        .checked_sub(minimum)
        .and_then(|span| usize::try_from(span).ok())
        .filter(|&span| span < MAX_ID_SPAN)
    else {
        return Ok(None);
    };
    let mut slots = [None; MAX_ID_SPAN];
    let slots = &mut slots[..=span];
    for (index, &id) in ids.iter().enumerate() {
        // The checked span proves each selected ID's subtraction and conversion.
        let offset = usize::try_from(id - minimum).expect("feed ID within checked span");
        slots[offset] = Some(index);
    }
    let selected_slot = |id: i64| {
        id.checked_sub(minimum)
            .and_then(|offset| usize::try_from(offset).ok())
            .and_then(|offset| slots.get(offset))
            .copied()
            .flatten()
    };
    let mut likes = [0_i64; 20];
    {
        // Aggregate inside SQLite: the bounded ID range returns at most 256 groups.
        let mut rows = statements.likes.query(params![minimum, maximum])?;
        while let Some(row) = rows.next()? {
            if let Some(slot) = selected_slot(row.get(0)?) {
                likes[slot] = row.get(1)?;
            }
        }
    }
    statements.scratch.clear();
    let mut segments = [None; 20];
    {
        let mut rows = statements.rows.query(params![minimum, maximum])?;
        while let Some(row) = rows.next()? {
            if let Some(slot) = selected_slot(row.get(0)?) {
                let start = statements.scratch.len();
                decode_post_with_likes(row, likes[slot])?.write_json(&mut statements.scratch)?;
                segments[slot] = Some((start, statements.scratch.len()));
            }
        }
    }
    if segments[..len].iter().any(Option::is_none) {
        // LIMIT in the original query follows the users join. A missing author
        // can admit a later post, so a posts-only ID scan must fall back exactly.
        return Ok(None);
    }
    let mut output = Vec::with_capacity(*capacity);
    output.extend_from_slice(b"{\"posts\":[");
    for (index, segment) in segments[..len].iter().enumerate() {
        if index != 0 {
            output.push(b',');
        }
        let (start, end) = segment.expect("joined feed row");
        output.extend_from_slice(&statements.scratch[start..end]);
    }
    output.extend_from_slice(b"]}");
    *capacity = output.capacity();
    Ok(Some(Bytes::from(output)))
}

fn read_post(
    statement: &mut rusqlite::Statement<'_>,
    id: i64,
    capacity: &mut usize,
) -> Result<Option<Vec<u8>>, DbError> {
    let mut rows = statement.query([id])?;
    let Some(row) = rows.next()? else {
        return Ok(None);
    };
    let mut output = Vec::with_capacity(*capacity);
    output.extend_from_slice(b"{\"post\":");
    decode_post(row)?.write_json(&mut output)?;
    output.push(b'}');
    *capacity = output.capacity();
    Ok(Some(output))
}

fn insert_post(
    statement: &mut rusqlite::Statement<'_>,
    user_id: i64,
    body: Bytes,
    username: String,
    capacity: &mut usize,
) -> Result<Vec<u8>, DbError> {
    let body_text = std::str::from_utf8(&body)?;
    let output = {
        let mut rows = statement.query(params![user_id, body_text])?;
        let row = rows.next()?.ok_or(rusqlite::Error::QueryReturnedNoRows)?;
        let post = Post {
            id: row.get(0)?,
            body: body_text,
            created_at: row.get_ref(1)?.as_str().map_err(rusqlite::Error::from)?,
            author: &username,
            like_count: 0,
        };
        let mut output = Vec::with_capacity(*capacity);
        output.extend_from_slice(b"{\"post\":");
        post.write_json(&mut output)?;
        output.push(b'}');
        // Complete RETURNING before committing; borrowed row data is no longer used.
        rows.next()?;
        output
    };
    *capacity = output.capacity();
    Ok(output)
}

fn insert_like(
    exists_statement: &mut rusqlite::Statement<'_>,
    insert_statement: &mut rusqlite::Statement<'_>,
    user_id: i64,
    post_id: i64,
) -> Result<Option<bool>, DbError> {
    let inserted = match insert_statement.execute(params![user_id, post_id]) {
        Ok(inserted) => inserted,
        Err(error) => {
            // Let SQLite's foreign key check handle successful inserts. Only
            // classify a missing post after failure; invalid users stay errors.
            if error.sqlite_error().is_some_and(|error| {
                error.extended_code == rusqlite::ffi::SQLITE_CONSTRAINT_FOREIGNKEY
            }) {
                let exists: bool = exists_statement.query_row([post_id], |row| row.get(0))?;
                if !exists {
                    return Ok(None);
                }
            }
            return Err(error.into());
        }
    };
    if inserted != 0 {
        return Ok(Some(false));
    }

    // A no-op insert normally means a duplicate. Check the post so pre-existing
    // orphan likes still return 404.
    let exists: bool = exists_statement.query_row([post_id], |row| row.get(0))?;
    if !exists {
        return Ok(None);
    }
    Ok(Some(true))
}

#[cfg(test)]
include!("tests/db.rs");
