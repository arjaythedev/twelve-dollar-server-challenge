//! epoll HTTP front end plus one FIFO SQLite worker.
//!
//! Network threads never touch SQLite and never hold a database lock. Every
//! parsed request becomes a Job on one ordered channel. The single worker
//! drains that channel, executes each batch in arrival order, commits once,
//! and only then publishes the batch's replies. Replies travel back over a
//! per-reactor channel and wake the epoll loop through an eventfd.

mod db;
mod lufp;
mod net;

use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::thread;
use std::time::Instant;

pub(crate) enum Op {
    Read(Option<i64>),
    Health,
    Create {
        user: i64,
        username: Box<str>,
        body: Box<str>,
    },
    Like {
        user: i64,
        post: i64,
    },
    Immediate {
        status: u16,
        body: Vec<u8>,
    },
    Continue,
}

pub(crate) struct Job {
    pub reactor: u16,
    pub fd: u32,
    pub generation: u64,
    pub close: bool,
    pub op: Op,
}

pub(crate) struct Reply {
    pub reactor: u16,
    pub fd: u32,
    pub generation: u64,
    pub status: u16,
    pub body: Vec<u8>,
    pub close: bool,
    pub raw: bool,
    // A health reply describes the SELECT 1 it already ran; a later batch
    // COMMIT failure must not replace 200/503 with a generic 500.
    pub preserve: bool,
}

pub(crate) const CONTINUE: &[u8] = b"HTTP/1.1 100 Continue\r\n\r\n";
const ERROR_500: &[u8] = b"{\"error\":\"internal server error\"}";
const MAX_BATCH: usize = 128;

fn env_parse<T: std::str::FromStr>(name: &str, default: T) -> T {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

pub fn number(n: u64, out: &mut Vec<u8>) {
    out.extend_from_slice(itoa::Buffer::new().format(n).as_bytes());
}

// Vectorize the common ASCII case without ever reading beyond the provided slice.
#[inline]
fn plain16(b: &[u8]) -> bool {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        use std::arch::x86_64::*;
        let v = _mm_loadu_si128(b.as_ptr().cast());
        let bad = _mm_or_si128(
            _mm_or_si128(
                _mm_cmpeq_epi8(v, _mm_set1_epi8(34)),
                _mm_cmpeq_epi8(v, _mm_set1_epi8(92)),
            ),
            _mm_min_epu8(v, _mm_set1_epi8(31)),
        );
        _mm_movemask_epi8(bad) == 0
    }
    #[cfg(target_arch = "aarch64")]
    unsafe {
        use std::arch::aarch64::*;
        let v = vld1q_u8(b.as_ptr());
        vmaxvq_u8(vorrq_u8(
            vorrq_u8(vceqq_u8(v, vdupq_n_u8(34)), vceqq_u8(v, vdupq_n_u8(92))),
            vcltq_u8(v, vdupq_n_u8(32)),
        )) == 0
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
    {
        b[..16].iter().all(|&c| c >= 32 && c != b'"' && c != b'\\')
    }
}

pub fn json_string(b: &[u8], out: &mut Vec<u8>) {
    out.push(b'"');
    let mut i = 0;
    let mut start = 0;
    while i < b.len() {
        if i + 16 <= b.len() && plain16(&b[i..]) {
            i += 16;
            continue;
        }
        let c = b[i];
        if c < 32 || c == b'"' || c == b'\\' {
            out.extend_from_slice(&b[start..i]);
            match c {
                b'"' => out.extend_from_slice(b"\\\""),
                b'\\' => out.extend_from_slice(b"\\\\"),
                b'\n' => out.extend_from_slice(b"\\n"),
                b'\r' => out.extend_from_slice(b"\\r"),
                b'\t' => out.extend_from_slice(b"\\t"),
                _ => {
                    const HEX: &[u8] = b"0123456789abcdef";
                    out.extend_from_slice(&[
                        b'\\',
                        b'u',
                        b'0',
                        b'0',
                        HEX[(c >> 4) as usize],
                        HEX[(c & 15) as usize],
                    ]);
                }
            }
            start = i + 1;
        }
        i += 1;
    }
    out.extend_from_slice(&b[start..]);
    out.push(b'"');
}

pub fn error(out: &mut Vec<u8>, status: u16, msg: &str) -> u16 {
    out.clear();
    out.extend_from_slice(b"{\"error\":");
    json_string(msg.as_bytes(), out);
    out.push(b'}');
    status
}

fn notify(eventfd: i32) {
    let one = 1u64.to_ne_bytes();
    unsafe {
        libc::write(eventfd, one.as_ptr().cast(), one.len());
    }
}

fn db_worker(
    jobs: Receiver<Job>,
    replies: Vec<(Sender<Reply>, i32)>,
    start: Instant,
    path: String,
    ready: mpsc::SyncSender<()>,
) {
    let mut db = db::Db::open(&path, true);
    let _ = ready.send(());

    let mut batch = Vec::with_capacity(MAX_BATCH);
    let mut out = Vec::with_capacity(512);
    loop {
        let first = match jobs.recv() {
            Ok(job) => job,
            Err(_) => break,
        };
        batch.push(first);
        let mut disconnected = false;
        while batch.len() < MAX_BATCH {
            match jobs.try_recv() {
                Ok(job) => batch.push(job),
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    disconnected = true;
                    break;
                }
            }
        }

        // The FIFO drain above defines the order. A batch shares one deferred
        // transaction and one commit; no response leaves the process first.
        let needs_db = batch
            .iter()
            .any(|j| !matches!(j.op, Op::Immediate { .. } | Op::Continue));
        let mut ok = !needs_db || db.begin();
        let mut responses = Vec::with_capacity(batch.len());

        for job in batch.iter_mut() {
            out.clear();
            // Health is still a SELECT 1 when an earlier BEGIN failed; its
            // result describes the database handle, not the batch commit.
            let (status, raw, preserve) = if !ok && !matches!(&job.op, Op::Health) {
                out.extend_from_slice(ERROR_500);
                (500, false, false)
            } else {
                match &mut job.op {
                    Op::Read(id) => (db.read(*id, &mut out), false, false),
                    Op::Health => (
                        db.health(start.elapsed().as_secs(), &mut out),
                        false,
                        true,
                    ),
                    Op::Create {
                        user,
                        username,
                        body,
                    } => (db.create(*user, username, body, &mut out), false, false),
                    Op::Like { user, post } => (db.like(*user, *post, &mut out), false, false),
                    Op::Immediate { status, body } => {
                        out = std::mem::take(body);
                        (*status, false, false)
                    }
                    Op::Continue => {
                        out.extend_from_slice(CONTINUE);
                        (0, true, false)
                    }
                }
            };
            responses.push(Reply {
                reactor: job.reactor,
                fd: job.fd,
                generation: job.generation,
                status,
                body: std::mem::take(&mut out),
                close: job.close,
                raw,
                preserve,
            });
        }

        if needs_db && !db.commit() {
            ok = false;
        }
        if !ok {
            for reply in &mut responses {
                if !reply.raw && !reply.preserve {
                    reply.status = 500;
                    reply.body.clear();
                    reply.body.extend_from_slice(ERROR_500);
                }
            }
        }

        // One eventfd write per reactor per batch, after every reply is queued.
        let mut wake = 0u64;
        for reply in responses {
            let idx = reply.reactor as usize;
            if let Some((tx, _)) = replies.get(idx) {
                if tx.send(reply).is_ok() {
                    wake |= 1u64 << idx;
                }
            }
        }
        for (idx, (_, eventfd)) in replies.iter().enumerate() {
            if wake & (1u64 << idx) != 0 {
                notify(*eventfd);
            }
        }
        db.checkpoint();
        batch.clear();
        // On the one-vCPU scoring box the reactor and this worker share a core.
        // Give the reactor a scheduling point after every batch so replies are
        // written before the next full queue is drained.
        std::thread::yield_now();
        if disconnected {
            break;
        }
    }
}

fn main() {
    let path = std::env::var("SQLITE_PATH").expect("SQLITE_PATH required");
    let secret = std::env::var("JWT_SECRET").expect("JWT_SECRET required");
    let host = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into());
    let port: u16 = env_parse("PORT", 80);
    let address = format!("{host}:{port}");
    // The scored box is one vCPU. Extra reactors are opt-in for larger
    // containers; all of them still feed the same ordered SQLite worker.
    let reactors: usize = env_parse("REACTORS", 1);
    let reactors = reactors.clamp(1, 32);

    let (job_tx, job_rx) = mpsc::channel::<Job>();
    let (ready_tx, ready_rx) = mpsc::sync_channel::<()>(0);
    let start = Instant::now();

    let mut reply_txs = Vec::with_capacity(reactors);
    let mut reply_ends = Vec::with_capacity(reactors);
    for _ in 0..reactors {
        let (reply_tx, reply_rx) = mpsc::channel::<Reply>();
        let eventfd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        assert!(eventfd >= 0, "eventfd");
        reply_txs.push((reply_tx, eventfd));
        reply_ends.push((reply_rx, eventfd));
    }

    let db_thread = thread::Builder::new()
        .name("sqlite-fifo".into())
        .spawn(move || db_worker(job_rx, reply_txs, start, path, ready_tx))
        .expect("spawn sqlite worker");
    // The worker opens SQLite before any reactor starts accepting.
    let _ = ready_rx.recv();

    let mut handles = Vec::with_capacity(reactors);
    for (id, (reply_rx, eventfd)) in reply_ends.into_iter().enumerate() {
        let jobs = job_tx.clone();
        let address = address.clone();
        let secret = secret.clone();
        handles.push(
            thread::Builder::new()
                .name(format!("epoll-{id}"))
                .spawn(move || {
                    if let Err(err) = net::run(
                        id as u16,
                        &address,
                        &secret,
                        jobs,
                        reply_rx,
                        eventfd,
                    ) {
                        eprintln!("reactor {id}: {err}");
                        std::process::exit(1);
                    }
                })
                .expect("spawn epoll reactor"),
        );
    }
    drop(job_tx);

    eprintln!(
        "listening {address}; {reactors} epoll reactor(s), 1 ordered SQLite worker; WAL/NORMAL"
    );
    for handle in handles {
        let _ = handle.join();
    }
    let _ = db_thread.join();
}
