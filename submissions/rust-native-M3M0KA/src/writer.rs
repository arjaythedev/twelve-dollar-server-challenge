//! The issuer's write path, split off the ring loop: accepted POSTs park
//! their (already validated) job in a bounded queue and a single dedicated
//! thread executes them on its own SQLite connection, gathers up to 20 ms,
//! commits once, and hands complete responses back through a ready queue.
//!
//! Ordering and visibility rules, unchanged from the merged design:
//! - a queued write's reply is submitted only after the commit that contains
//!   it, so an acknowledged write always survives a kill -9;
//! - a connection with a queued write is not re-armed for further requests
//!   until that write commits (pipeline order is preserved);
//! - reads issued after a reply reflect committed state (the read snapshot
//!   opens after the reply was submitted).
//!
//! Bounds: the queue holds at most 8,192 jobs; a gather batch executes at
//! most 1,024; each job trades one lane/slot/generation identity.

use std::collections::VecDeque;
use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar};

use crate::app::WriteJob;
use crate::db::{WriteConn, LikeRaw};

extern "C" {
    fn eventfd(initval: u32, flags: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut c_void, n: usize) -> isize;
    fn write(fd: c_int, buf: *const c_void, n: usize) -> isize;
}

/// eventfd(0, CLOEXEC|NONBLOCK).
pub fn eventfd_new() -> c_int {
    unsafe { eventfd(0, 0o2000000 | 0o4000) }
}

/// One read clears a counter-style eventfd; EAGAIN is a no-op.
pub fn eventdrain(fd: c_int) {
    let mut v: u64 = 0;
    unsafe {
        read(fd, &mut v as *mut u64 as *mut c_void, 8);
    }
}

/// Signals the issuer poll registered with this fd.
pub fn eventpulse(fd: c_int) {
    let v: u64 = 1;
    unsafe {
        write(fd, &v as *const u64 as *const c_void, 8);
    }
}

const QUEUE_CAP: usize = 8192;
const GATHER_MAX_JOBS: usize = 1024;
const GATHER_WINDOW: std::time::Duration = std::time::Duration::from_millis(20);

/// What a completed write hands back to the issuer.
pub struct Reply {
    pub lane: u32,
    pub slot: u32,
    pub gen: u32,
    /// Full HTTP response (status line through body).
    pub bytes: Vec<u8>,
    /// True when the connection must close (commit failed).
    pub close: bool,
}

struct Queued {
    lane: u32,
    slot: u32,
    gen: u32,
    job: crate::app::WriteJob,
}

pub struct Shared {
    jobs: MutexJobs,
    ready: MutexReady,
    commit_epoch: AtomicU64,
    writer_alive: AtomicBool,
    /// The issuer's wake fd (eventfd), written after every commit batch.
    wake_fd: std::sync::atomic::AtomicI32,
}

struct MutexJobs {
    q: std::sync::Mutex<VecDeque<Queued>>,
    cv: std::sync::Condvar,
}

struct MutexReady {
    q: std::sync::Mutex<VecDeque<Reply>>,
}

impl Default for Shared {
    fn default() -> Shared {
        Shared::new()
    }
}

impl Shared {
    fn new() -> Shared {
        Shared {
            jobs: MutexJobs {
                q: std::sync::Mutex::new(VecDeque::new()),
                cv: std::sync::Condvar::new(),
            },
            ready: MutexReady {
                q: std::sync::Mutex::new(VecDeque::new()),
            },
            commit_epoch: AtomicU64::new(0),
            writer_alive: AtomicBool::new(false),
            wake_fd: std::sync::atomic::AtomicI32::new(-1),
        }
    }
}

/// The issuer's handle: the job queue, the ready queue, and the wake fd the
/// issuer registers in its own poll.
pub struct IssuerQueue {
    sh: Arc<Shared>,
    pub fd: c_int,
}

impl IssuerQueue {
    /// Creates the pair and spawns the writer thread on its own connection.
    pub fn new(db_path: &str) -> std::io::Result<IssuerQueue> {
        let fd = eventfd_new();
        if fd < 0 {
            return Err(std::io::Error::last_os_error());
        }
        let conn = WriteConn::open(db_path).ok_or_else(|| {
            std::io::Error::new(std::io::ErrorKind::Other, "writer connection failed")
        })?;
        let sh = Arc::new(Shared::default());
        sh.wake_fd.store(fd, Ordering::Release);
        spawn(sh.clone(), conn)?;
        Ok(IssuerQueue { sh, fd })
    }

    /// Publishes a parked write. `false` = queue full: the caller reports
    /// 503 and closes, exactly like a failed batch commit.
    pub fn enqueue(&self, lane: u32, slot: u32, gen: u32, job: WriteJob) -> bool {
        let mut q = self.sh.jobs.q.lock().unwrap();
        if q.len() >= QUEUE_CAP {
            return false;
        }
        q.push_back(Queued { lane, slot, gen, job });
        self.sh.jobs.cv.notify_one();
        true
    }

    /// Drains every committed reply published so far.
    pub fn drain(&self) -> Vec<Reply> {
        let mut q = self.sh.ready.q.lock().unwrap();
        std::mem::take(&mut *q).into_iter().collect()
    }
}


fn respond_head_bytes(code: u16, body: Vec<u8>) -> Vec<u8> {
    let mut v = Vec::with_capacity(96 + body.len());
    v.extend_from_slice(b"HTTP/1.1 ");
    crate::http::put_i64(&mut v, code as i64);
    v.extend_from_slice(b" ");
    v.extend_from_slice(crate::http::reason(code).as_bytes());
    v.extend_from_slice(b"\r\nContent-Type: application/json\r\nContent-Length: ");
    crate::http::put_i64(&mut v, body.len() as i64);
    v.extend_from_slice(b"\r\n\r\n");
    v.extend_from_slice(&body);
    v
}

fn build_create_reply(id: i64, body: &[u8], created: &[u8], author: &[u8]) -> Vec<u8> {
    let mut inner = Vec::with_capacity(1024);
    inner.extend_from_slice(b"{\"post\":{\"id\":");
        crate::http::put_i64(&mut inner, id);
        inner.extend_from_slice(b",\"body\":");
        crate::http::put_jstr(&mut inner, body);
        inner.extend_from_slice(b",\"created_at\":");
        crate::http::put_jstr(&mut inner, created);
        inner.extend_from_slice(b",\"author\":");
        crate::http::put_jstr(&mut inner, author);
        inner.extend_from_slice(b",\"like_count\":0}}");
        respond_head_bytes(201, inner)
}

fn build_like_reply(already: bool, id: i64) -> Vec<u8> {
    let mut inner = Vec::with_capacity(96);
    inner.extend_from_slice(
        if already {
            b"{\"liked\":true,\"already_liked\":true,\"post_id\":"
        } else {
            b"{\"liked\":true,\"already_liked\":false,\"post_id\":"
        },
    );
    crate::http::put_i64(&mut inner, id);
    inner.push(b'}');
    respond_head_bytes(if already { 200 } else { 201 }, inner)
}

pub fn spawn(shared: Arc<Shared>, conn: WriteConn) -> std::io::Result<()> {
    shared.writer_alive.store(true, Ordering::Release);
    std::thread::Builder::new()
        .stack_size(512 * 1024)
        .spawn(move || run(shared, conn))?;
    Ok(())
}

fn run(sh: Arc<Shared>, mut conn: WriteConn) {
    loop {
        let gathered = gather(&sh, GATHER_WINDOW);
        if gathered.is_empty() {
            // Idle: keep the writer connection's WAL from being touched while
            // the issuer serves reads.
            if sh.writer_alive.load(Ordering::Acquire) == false {
                return;
            }
            continue;
        }
        let failures = commit_batch(&mut conn, gathered, &sh);
        if failures != 0 {
            eprintln!("writer: batch commit failed for {failures} jobs: replies become 500");
        }
    }
}

fn gather(sh: &Arc<Shared>, window: std::time::Duration) -> Vec<Queued> {
    let mut q = sh.jobs.q.lock().unwrap();
    if q.is_empty() {
        let (locked, _) = sh.jobs.cv.wait_timeout(q, window).unwrap();
        q = locked;
    }
    let mut out: Vec<Queued> = std::mem::take(&mut *q).into_iter().collect();
    out.truncate(GATHER_MAX_JOBS);
    out
}

fn commit_batch(conn: &mut WriteConn, batch: Vec<Queued>, sh: &Arc<Shared>) -> usize {
    let batch_len = batch.len();
    let mut replies: Vec<Reply> = Vec::with_capacity(batch_len);
    let mut failures: usize = 0;
    if !conn.begin_write() {
        failures = batch_len;
        for jq in batch {
            replies.push(Reply {
                lane: jq.lane,
                slot: jq.slot,
                gen: jq.gen,
                bytes: respond_head_bytes(500, b"{\"error\":\"database busy\"}".to_vec()),
                close: true,
            });
        }
        publish(sh, replies);
        return failures;
    }
    for jq in batch {
        match jq.job {
            crate::app::WriteJob::Create(uid, name, body) => match conn.create(uid, &body) {
                Some((id, created)) => {
                    replies.push(Reply {
                        lane: jq.lane,
                        slot: jq.slot,
                        gen: jq.gen,
                        bytes: build_create_reply(id, &body, &created, &name),
                        close: false,
                    });
                }
                None => {
                    failures += 1;
                    replies.push(Reply {
                        lane: jq.lane,
                        slot: jq.slot,
                        gen: jq.gen,
                        bytes: respond_head_bytes(500, b"{\"error\":\"internal error\"}".to_vec()),
                        close: false,
                    });
                }
            },
            crate::app::WriteJob::Like(uid, id) => match conn.like(uid, id) {
                crate::db::LikeRaw::Created => {
                    replies.push(Reply {
                        lane: jq.lane,
                        slot: jq.slot,
                        gen: jq.gen,
                        bytes: build_like_reply(false, id),
                        close: false,
                    });
                }
                crate::db::LikeRaw::AlreadyLiked => {
                    replies.push(Reply {
                        lane: jq.lane,
                        slot: jq.slot,
                        gen: jq.gen,
                        bytes: build_like_reply(true, id),
                        close: false,
                    });
                }
                crate::db::LikeRaw::Missing => {
                    replies.push(Reply {
                        lane: jq.lane,
                        slot: jq.slot,
                        gen: jq.gen,
                        bytes: respond_head_bytes(404, b"{\"error\":\"post not found\"}".to_vec()),
                        close: false,
                    });
                }
                crate::db::LikeRaw::Error => {
                    failures += 1;
                    replies.push(Reply {
                        lane: jq.lane,
                        slot: jq.slot,
                        gen: jq.gen,
                        bytes: respond_head_bytes(500, b"{\"error\":\"internal error\"}".to_vec()),
                        close: false,
                    });
                }
            },
        }
    }
    let committed = conn.commit();
    if !committed {
        failures = batch_len;
        for r in replies.iter_mut() {
            r.bytes = respond_head_bytes(500, b"{\"error\":\"database busy\"}".to_vec());
            r.close = true;
        }
    }
    publish(sh, replies);
    sh.commit_epoch.fetch_add(1, Ordering::Release);
    failures
}

fn publish(sh: &Arc<Shared>, replies: Vec<Reply>) {
    if replies.is_empty() {
        return;
    }
    let mut q = sh.ready.q.lock().unwrap();
    for r in replies {
        q.push_back(r);
    }
    drop(q);
    let fd = sh.wake_fd.load(Ordering::Acquire);
    if fd >= 0 {
        eventpulse(fd);
    }
}
