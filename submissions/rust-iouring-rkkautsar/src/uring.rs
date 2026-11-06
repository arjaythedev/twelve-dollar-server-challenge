use std::alloc::{alloc_zeroed, dealloc, Layout};
use std::collections::HashMap;
use std::io;
use std::mem::size_of;
use std::net::{SocketAddr, ToSocketAddrs};
use std::os::fd::{AsRawFd, RawFd};
use std::sync::mpsc::{Receiver, Sender};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::OnceLock;
use std::sync::Arc;
use std::time::{Duration, Instant};

use io_uring::{cqueue, opcode, squeue, types, IoUring};

use super::{parse_request, response_bytes, App, Handled, ParseResult};
use crate::reader::PostResponse;
use crate::stats;
use crate::writer::{WriteJob, WriteResult};

const TAG_ACCEPT: u64 = 1;
const TAG_READ: u64 = 2;
const TAG_WRITE: u64 = 3;
const TAG_CLOSE: u64 = 4;
const TAG_SHIFT: u64 = 52;
const GEN_SHIFT: u64 = 20;
const SLOT_MASK: u64 = (1 << 20) - 1;

const FILE_TABLE: u32 = 65_000;
const RING_ENTRIES: u32 = 4_096;
const CQES_PER_RING: usize = 2048;
const READ_QUEUE_CAP: usize = usize::MAX;
const FEED_ANSWERS_PER_PASS: usize = usize::MAX;
const POST_ANSWERS_PER_PASS: usize = usize::MAX;
const READ_BUF: usize = 2_048;
const BUF_READ: usize = 512;
const BUF_RING_ENTRIES: u16 = 32_768;
const BUF_GROUP: u16 = 1;

#[derive(Clone, Copy)]
enum Diag { Process, Feed, Post, Write }

static DIAG_N: [AtomicU64; 4] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
];
static DIAG_NS: [AtomicU64; 4] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
];
static DIAG_MAX: [AtomicU64; 4] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
];
static ACCEPTS: AtomicU64 = AtomicU64::new(0);
static CLOSES: AtomicU64 = AtomicU64::new(0);
static EOFS: AtomicU64 = AtomicU64::new(0);
static READ_ERRORS: AtomicU64 = AtomicU64::new(0);
static ACTIVE: AtomicU64 = AtomicU64::new(0);

fn diag_on() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("DIAG").is_ok())
}

fn diag_start() -> Option<Instant> {
    if diag_on() { Some(Instant::now()) } else { None }
}

fn diag_end(kind: Diag, at: Option<Instant>) {
    let Some(at) = at else { return };
    let idx = kind as usize;
    let ns = at.elapsed().as_nanos() as u64;
    DIAG_N[idx].fetch_add(1, Ordering::Relaxed);
    DIAG_NS[idx].fetch_add(ns, Ordering::Relaxed);
    let mut old = DIAG_MAX[idx].load(Ordering::Relaxed);
    while ns > old {
        match DIAG_MAX[idx].compare_exchange_weak(old, ns, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(v) => old = v,
        }
    }
}

fn diag_tick(passes: u64, feed_q: usize, post_q: usize) {
    if !diag_on() || passes % 2048 != 0 {
        return;
    }
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    let utime = ru.ru_utime.tv_sec as u64 * 1_000_000 + ru.ru_utime.tv_usec as u64;
    let stime = ru.ru_stime.tv_sec as u64 * 1_000_000 + ru.ru_stime.tv_usec as u64;
    let mut out = String::new();
    for (name, i) in [("proc", 0), ("feed", 1), ("post", 2), ("write", 3)] {
        let n = DIAG_N[i].load(Ordering::Relaxed);
        let ns = DIAG_NS[i].load(Ordering::Relaxed);
        out.push_str(&format!(
            " {}_n={} {}_avg_us={} {}_max_us={}",
            name,
            n,
            name,
            if n == 0 { 0 } else { ns / n / 1000 },
            name,
            DIAG_MAX[i].load(Ordering::Relaxed) / 1000
        ));
    }
    eprintln!(
        "diag pass={} accepts={} closes={} eofs={} rerr={} active={} fq={} pq={} utime_us={} stime_us={}{}",
        passes,
        ACCEPTS.load(Ordering::Relaxed),
        CLOSES.load(Ordering::Relaxed),
        EOFS.load(Ordering::Relaxed),
        READ_ERRORS.load(Ordering::Relaxed),
        ACTIVE.load(Ordering::Relaxed),
        feed_q,
        post_q,
        utime,
        stime,
        out
    );
}

enum OutBuf {
    Empty,
    Owned(Vec<u8>, usize),
    Shared(Arc<Vec<u8>>, usize),
}

impl OutBuf {
    fn len(&self) -> usize {
        match self {
            OutBuf::Empty => 0,
            OutBuf::Owned(v, off) => v.len().saturating_sub(*off),
            OutBuf::Shared(v, off) => v.len().saturating_sub(*off),
        }
    }
    fn ptr(&self, off: usize) -> *const u8 {
        match self {
            OutBuf::Empty => std::ptr::null(),
            OutBuf::Owned(v, o) => unsafe { v.as_ptr().add(*o + off) },
            OutBuf::Shared(v, o) => unsafe { v.as_ptr().add(*o + off) },
        }
    }
    fn advance(&mut self, n: usize) {
        match self {
            OutBuf::Empty => {}
            OutBuf::Owned(_, off) | OutBuf::Shared(_, off) => *off += n,
        }
    }
}


struct Conn {
    input: Vec<u8>,
    read: Option<Box<[u8; READ_BUF]>>,
    out: OutBuf,
    gen: u32,
    multishot: bool,
    writing: bool,
    write_pending: bool,
    close_after: bool,
}

impl Conn {
    fn new(gen: u32, multishot: bool) -> Self {
        Self {
            input: Vec::new(),
            read: if multishot { None } else { Some(Box::new([0u8; READ_BUF])) },
            out: OutBuf::Empty,
            gen,
            multishot,
            writing: false,
            write_pending: false,
            close_after: false,
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct ReadJob {
    pub ring: usize,
    pub slot: u32,
    pub gen: u32,
    pub close: bool,
    pub at: Option<std::time::Instant>,
    pub kind: ReadKind,
}

#[derive(Clone, Copy)]
pub(crate) enum ReadKind {
    Feed,
    Post(i64),
}

#[repr(C)]
#[derive(Clone, Copy)]
struct UringBuf {
    addr: u64,
    len: u32,
    bid: u16,
    resv: u16,
}

struct BufRing {
    mem: *mut u8,
    layout: Layout,
    bufs: Box<[u8]>,
    tail: u32,
    mask: u32,
}

impl BufRing {
    fn new(ring: &IoUring) -> io::Result<Self> {
        let entries = BUF_RING_ENTRIES;
        let layout = Layout::from_size_align(entries as usize * 16, 4096)
            .map_err(|_| io::Error::from_raw_os_error(libc::ENOMEM))?;
        let mem = unsafe { alloc_zeroed(layout) };
        if mem.is_null() {
            return Err(io::Error::from_raw_os_error(libc::ENOMEM));
        }
        let bufs = vec![0u8; entries as usize * BUF_READ].into_boxed_slice();
        unsafe {
            if let Err(e) = ring.submitter().register_buf_ring_with_flags(
                mem as u64,
                entries,
                BUF_GROUP,
                0,
            ) {
                dealloc(mem, layout);
                return Err(e);
            }
        }
        let mut br = Self {
            mem,
            layout,
            bufs,
            tail: 0,
            mask: entries as u32 - 1,
        };
        for bid in 0..entries {
            let e = UringBuf {
                addr: unsafe { br.bufs.as_ptr().add(bid as usize * BUF_READ) as u64 },
                len: BUF_READ as u32,
                bid,
                resv: 0,
            };
            unsafe { std::ptr::write(br.mem.add(bid as usize * 16) as *mut UringBuf, e) };
        }
        br.tail = entries as u32;
        unsafe {
            (&*(br.mem.add(12) as *const AtomicU32)).store(br.tail << 16, Ordering::Release);
        }
        Ok(br)
    }

    fn put(&mut self, bid: u16) {
        if (bid as usize) >= self.bufs.len() / BUF_READ {
            return;
        }
        let slot = (self.tail & self.mask) as usize ;
        let e = UringBuf {
            addr: unsafe { self.bufs.as_ptr().add(bid as usize * BUF_READ) as u64 },
            len: BUF_READ as u32,
            bid,
            resv: 0,
        };
        unsafe { std::ptr::write(self.mem.add(slot * 16) as *mut UringBuf, e) };
        self.tail = (self.tail.wrapping_add(1)) & 0xffff;
        unsafe {
            (&*(self.mem.add(12) as *const AtomicU32)).store(self.tail << 16, Ordering::Release);
        }
    }
}

impl Drop for BufRing {
    fn drop(&mut self) {
        unsafe { dealloc(self.mem, self.layout) };
    }
}

struct RingState {
    ring: IoUring,
    listener: i32,
    conns: HashMap<u32, Conn>,
    multishot: bool,
    buf_ring: Option<BufRing>,
    next_gen: u32,
    last_cqes: usize,
    accepting: bool,
    accept_paused: bool,
    accept_resume: Option<Instant>,
}

#[inline]
fn ud(slot: u32, gen: u32, tag: u64) -> u64 {
    (slot as u64) | ((gen as u64) << GEN_SHIFT) | (tag << TAG_SHIFT)
}

#[inline]
fn split_ud(v: u64) -> (u32, u32, u64) {
    (
        (v & SLOT_MASK) as u32,
        ((v >> GEN_SHIFT) & 0xffff_ffff) as u32,
        v >> TAG_SHIFT,
    )
}

fn push(ring: &mut IoUring, entry: squeue::Entry) -> io::Result<()> {
    for _ in 0..8 {
        {
            let mut sq = ring.submission();
            if unsafe { sq.push(&entry) }.is_ok() {
                sq.sync();
                return Ok(());
            }
        }
        ring.submit()?;
    }
    Err(io::Error::from_raw_os_error(libc::EBUSY))
}

fn make_listener(addr: &str) -> io::Result<i32> {
    let sa: SocketAddr = match addr.parse() {
        Ok(v) => v,
        Err(_) => addr
            .to_socket_addrs()?
            .find(|v| v.is_ipv4())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "no IPv4 address"))?,
    };
    let SocketAddr::V4(v4) = sa else {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "IPv4 required"));
    };
    let fd = unsafe {
        libc::socket(
            libc::AF_INET,
            libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            0,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let one: libc::c_int = 1;
    unsafe {
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_REUSEADDR,
            &one as *const _ as *const libc::c_void,
            size_of::<libc::c_int>() as libc::socklen_t,
        );
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            15,
            &one as *const _ as *const libc::c_void,
            size_of::<libc::c_int>() as libc::socklen_t,
        );
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_NODELAY,
            &one as *const _ as *const libc::c_void,
            size_of::<libc::c_int>() as libc::socklen_t,
        );
    }
    let sin = libc::sockaddr_in {
        sin_family: libc::AF_INET as libc::sa_family_t,
        sin_port: v4.port().to_be(),
        sin_addr: libc::in_addr {
            s_addr: u32::from_ne_bytes(v4.ip().octets()),
        },
        sin_zero: [0; 8],
    };
    let rc = unsafe {
        libc::bind(
            fd,
            &sin as *const _ as *const libc::sockaddr,
            size_of::<libc::sockaddr_in>() as libc::socklen_t,
        )
    };
    if rc < 0 {
        let err = io::Error::last_os_error();
        unsafe {
            libc::close(fd);
        }
        return Err(err);
    }
    if unsafe { libc::listen(fd, 65_535) } < 0 {
        let err = io::Error::last_os_error();
        unsafe {
            libc::close(fd);
        }
        return Err(err);
    }
    Ok(fd)
}

impl RingState {
    fn new(addr: &str) -> io::Result<Self> {
        let listener = make_listener(addr)?;
        let ring = {
            let mut last = None;
            let mut ring = None;
            for attempt in 0..6 {
                let mut builder = IoUring::builder();
                builder.setup_single_issuer().setup_coop_taskrun().setup_cqsize(8_192);
                match builder.build(RING_ENTRIES) {
                    Ok(r) => {
                        ring = Some(r);
                        break;
                    }
                    Err(e) => {
                        let retry = e.raw_os_error() == Some(libc::ENOMEM) && attempt < 5;
                        last = Some(e);
                        if !retry {
                            break;
                        }
                        std::thread::sleep(Duration::from_millis(20));
                    }
                }
            }
            match ring {
                Some(r) => r,
                None => {
                    unsafe {
                        libc::close(listener);
                    }
                    return Err(last.unwrap_or_else(|| io::Error::from_raw_os_error(libc::ENOMEM)));
                }
            }
        };
        if let Err(e) = ring.submitter().register_files_sparse(FILE_TABLE) {
            unsafe {
                libc::close(listener);
            }
            return Err(e);
        }
        let multishot = std::env::var("MULTISHOT")
            .map(|v| v != "0")
            .unwrap_or(true);
        let buf_ring = if multishot {
            match BufRing::new(&ring) {
                Ok(b) => Some(b),
                Err(e) => {
                    unsafe { libc::close(listener) };
                    return Err(e);
                }
            }
        } else {
            None
        };
        let mut state = Self {
            ring,
            listener,
            conns: HashMap::with_capacity(32_768),
            multishot,
            buf_ring,
            next_gen: 1,
            last_cqes: 0,
            accepting: false,
            accept_paused: false,
            accept_resume: None,
        };
        state.arm_accept()?;
        state.ring.submit()?;
        Ok(state)
    }

    fn arm_accept(&mut self) -> io::Result<()> {
        if self.accepting {
            return Ok(());
        }
        let entry = opcode::AcceptMulti::new(types::Fd(self.listener))
            .allocate_file_index(true)
            .build()
            .user_data(TAG_ACCEPT);
        push(&mut self.ring, entry)?;
        self.accepting = true;
        Ok(())
    }
}

fn arm_read(ring: &mut IoUring, slot: u32, c: &mut Conn) -> io::Result<()> {
    if c.close_after {
        return Ok(());
    }
    let entry = if c.multishot {
        opcode::RecvMulti::new(types::Fixed(slot), BUF_GROUP)
            .len(0)
            .build()
    } else {
        let buf = c.read.as_mut().unwrap();
        opcode::Recv::new(types::Fixed(slot), buf.as_mut_ptr(), READ_BUF as u32).build()
    };
    push(ring, entry.user_data(ud(slot, c.gen, TAG_READ)))
}

fn arm_write(ring: &mut IoUring, slot: u32, c: &mut Conn) -> io::Result<()> {
    if c.writing {
        return Ok(());
    }
    let len = c.out.len();
    if len == 0 {
        return Ok(());
    }
    let ptr = c.out.ptr(0);
    c.writing = true;
    let entry = opcode::Send::new(types::Fixed(slot), ptr, len as u32)
        .build()
        .user_data(ud(slot, c.gen, TAG_WRITE));
    push(ring, entry)
}

fn arm_close(ring: &mut IoUring, slot: u32, gen: u32) -> io::Result<()> {
    let entry = opcode::Close::new(types::Fixed(slot))
        .build()
        .user_data(ud(slot, gen, TAG_CLOSE));
    push(ring, entry)
}

fn close_packet(normal: &[u8]) -> Vec<u8> {
    let Some(h) = normal.windows(4).position(|w| w == b"\r\n\r\n") else {
        return normal.to_vec();
    };
    let mut out = Vec::with_capacity(normal.len() + 20);
    out.extend_from_slice(&normal[..h]);
    out.extend_from_slice(b"\r\nConnection: close\r\n\r\n");
    out.extend_from_slice(&normal[h + 4..]);
    out
}

fn queue_reply(c: &mut Conn, status: u16, body: &[u8], close: bool) {
    c.out = OutBuf::Owned(response_bytes(status, body, close), 0);
    if close {
        c.close_after = true;
    }
}

fn queue_shared(c: &mut Conn, packet: Arc<Vec<u8>>, close: bool) {
    c.out = OutBuf::Shared(packet, 0);
    if close {
        c.close_after = true;
    }
}

fn queue_owned(c: &mut Conn, packet: Vec<u8>, close: bool) {
    c.out = OutBuf::Owned(packet, 0);
    if close {
        c.close_after = true;
    }
}

fn finish_close(ring: &mut IoUring, conns: &mut HashMap<u32, Conn>, slot: u32) -> io::Result<()> {
    if let Some(c) = conns.remove(&slot) {
        CLOSES.fetch_add(1, Ordering::Relaxed);
        arm_close(ring, slot, c.gen)?;
    }
    Ok(())
}

fn process_input(
    ring: &mut IoUring,
    ring_idx: usize,
    slot: u32,
    c: &mut Conn,
    app: &mut App,
    write_tx: &Sender<WriteJob>,
    write_result_tx: &Sender<WriteResult>,
    write_event_fd: RawFd,
    feed_reads: &mut Vec<ReadJob>,
    post_reads: &mut Vec<ReadJob>,
) -> io::Result<()> {
    let at = diag_start();
    let r = process_input_inner(
        ring,
        ring_idx,
        slot,
        c,
        app,
        write_tx,
        write_result_tx,
        write_event_fd,
        feed_reads,
        post_reads,
    );
    diag_end(Diag::Process, at);
    r
}

fn process_input_inner(
    ring: &mut IoUring,
    ring_idx: usize,
    slot: u32,
    c: &mut Conn,
    app: &mut App,
    write_tx: &Sender<WriteJob>,
    write_result_tx: &Sender<WriteResult>,
    write_event_fd: RawFd,
    feed_reads: &mut Vec<ReadJob>,
    post_reads: &mut Vec<ReadJob>,
) -> io::Result<()> {
    if c.writing || c.write_pending || c.close_after {
        return Ok(());
    }
    match parse_request(&c.input) {
        ParseResult::Incomplete => {
            if c.multishot {
                Ok(())
            } else {
                arm_read(ring, slot, c)
            }
        }
        ParseResult::Bad => {
            queue_reply(c, 400, br#"{"error":"bad request"}"#, true);
            arm_write(ring, slot, c)
        }
        ParseResult::Complete(req, used) => {
            let close = req.close;
            let handled = app.handle(&req);
            drop(req);
            if used == c.input.len() {
                c.input.clear();
            } else {
                c.input.drain(..used);
            }
            if c.input.is_empty() && c.input.capacity() > 64 * 1024 {
                c.input.shrink_to(4 * 1024);
            }
            match handled {
                Handled::Reply(status, body) => {
                    queue_reply(c, status, &body, close);
                    arm_write(ring, slot, c)
                }
                Handled::Feed => {
                    feed_reads.push(ReadJob {
                        ring: ring_idx,
                        slot,
                        gen: c.gen,
                        close,
                        at: stats::mark(),
                        kind: ReadKind::Feed,
                    });
                    Ok(())
                }
                Handled::Post(id) => {
                    post_reads.push(ReadJob {
                        ring: ring_idx,
                        slot,
                        gen: c.gen,
                        close,
                        at: stats::mark(),
                        kind: ReadKind::Post(id),
                    });
                    Ok(())
                }
                Handled::Write(cmd) => {
                    let stat = match cmd.kind {
                        crate::WriteKind::Create { .. } => stats::CREATE,
                        crate::WriteKind::Like { .. } => stats::LIKE,
                    };
                    let job = WriteJob {
                        ring: ring_idx,
                        slot,
                        gen: c.gen,
                        close,
                        at: stats::mark(),
                        stat,
                        cmd,
                        result_tx: write_result_tx.clone(),
                        event_fd: write_event_fd,
                    };
                    if write_tx.send(job).is_err() {
                        queue_reply(c, 500, &super::error("internal server error"), true);
                        return arm_write(ring, slot, c);
                    }
                    c.write_pending = true;
                    Ok(())
                }
            }
        }
    }
}

fn after_write(
    ring: &mut IoUring,
    ring_idx: usize,
    slot: u32,
    c: &mut Conn,
    app: &mut App,
    write_tx: &Sender<WriteJob>,
    write_result_tx: &Sender<WriteResult>,
    write_event_fd: RawFd,
    feed_reads: &mut Vec<ReadJob>,
    post_reads: &mut Vec<ReadJob>,
    res: i32,
) -> io::Result<bool> {
    c.writing = false;
    c.out.advance(res as usize);
    if c.out.len() == 0 {
        c.out = OutBuf::Empty;
        if c.close_after {
            return Ok(true);
        }
        process_input(ring, ring_idx, slot, c, app, write_tx, write_result_tx, write_event_fd, feed_reads, post_reads)?;
        return Ok(false);
    }
    arm_write(ring, slot, c)?;
    Ok(false)
}

fn process_ring(
    ring_idx: usize,
    r: &mut RingState,
    app: &mut App,
    write_tx: &Sender<WriteJob>,
    write_result_tx: &Sender<WriteResult>,
    write_event_fd: RawFd,
    feed_reads: &mut Vec<ReadJob>,
    post_reads: &mut Vec<ReadJob>,
) -> io::Result<bool> {
    let mut cqe_count = 0usize;
    let mut full = false;
    loop {
        if feed_reads.len() + post_reads.len() >= READ_QUEUE_CAP {
            full = true;
            break;
        }
        if cqe_count >= CQES_PER_RING {
            break;
        }
        let next = {
            let mut completions = r.ring.completion();
            completions
                .next()
                .map(|cqe| (cqe.user_data(), cqe.result(), cqe.flags()))
        };
        let Some((user, res, flags)) = next else {
            break;
        };
        cqe_count += 1;
        if user == TAG_ACCEPT {
            if res >= 0 {
                let slot = res as u32;
                let gen = r.next_gen;
                r.next_gen = r.next_gen.wrapping_add(1).max(1);
                ACCEPTS.fetch_add(1, Ordering::Relaxed);
                r.conns.insert(slot, Conn::new(gen, r.multishot));
                if let Some(c) = r.conns.get_mut(&slot) {
                    arm_read(&mut r.ring, slot, c)?;
                }
            } else {
                r.accepting = false;
                let errno = -res;
                if errno == libc::ENFILE || errno == libc::EMFILE || errno == libc::ENOMEM {
                    r.accept_paused = true;
                    r.accept_resume = Some(Instant::now() + Duration::from_millis(100));
                } else if !r.accept_paused {
                    r.arm_accept()?;
                }
                continue;
            }
            if !cqueue::more(flags) && !r.accept_paused {
                r.accepting = false;
                r.arm_accept()?;
            }
            continue;
        }
        let (slot, gen, tag) = split_ud(user);
        if tag == TAG_READ {
            if !r.multishot {
                let close_now = if let Some(c) = r.conns.get_mut(&slot) {
                    if c.gen != gen {
                        false
                    } else if res <= 0 {
                        true
                    } else {
                        let n = res as usize;
                        if c.input.len() + n > (1 << 20) {
                            true
                        } else {
                            let data = &c.read.as_ref().unwrap()[..n];
                            c.input.extend_from_slice(data);
                            process_input(
                                &mut r.ring,
                                ring_idx,
                                slot,
                                c,
                                app,
                                write_tx,
                                write_result_tx,
                                write_event_fd,
                                feed_reads,
                                post_reads,
                            )?;
                            false
                        }
                    }
                } else {
                    false
                };
                if close_now {
                    let _ = finish_close(&mut r.ring, &mut r.conns, slot);
                }
                continue;
            }
            let bid = cqueue::buffer_select(flags);
            let close_now = if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen != gen {
                    if let Some(bid) = bid {
                        r.buf_ring.as_mut().unwrap().put(bid);
                    }
                    false
                } else if res > 0 {
                    match bid {
                        Some(bid) if (bid as usize) < BUF_RING_ENTRIES as usize => {
                            let n = res as usize;
                            if n > BUF_READ || c.input.len() + n > (1 << 20) {
                                r.buf_ring.as_mut().unwrap().put(bid);
                                true
                            } else {
                                let start = bid as usize * BUF_READ;
                                {
                                    let data = &r.buf_ring.as_ref().unwrap().bufs[start..start + n];
                                    c.input.extend_from_slice(data);
                                }
                                r.buf_ring.as_mut().unwrap().put(bid);
                                process_input(
                                    &mut r.ring,
                                    ring_idx,
                                    slot,
                                    c,
                                    app,
                                    write_tx,
                                    write_result_tx,
                                    write_event_fd,
                                    feed_reads,
                                    post_reads,
                                )?;
                                if !cqueue::more(flags) {
                                    let _ = arm_read(&mut r.ring, slot, c);
                                }
                                false
                            }
                        }
                        _ => true,
                    }
                } else {
                    if let Some(bid) = bid {
                        r.buf_ring.as_mut().unwrap().put(bid);
                    }
                    if res == -libc::ENOBUFS
                        || res == -libc::EAGAIN
                        || res == -libc::EINTR
                    {
                        let _ = arm_read(&mut r.ring, slot, c);
                        false
                    } else {
                        if res == 0 { EOFS.fetch_add(1, Ordering::Relaxed); }
                        else { READ_ERRORS.fetch_add(1, Ordering::Relaxed); }
                        true
                    }
                }
            } else {
                if let Some(bid) = bid {
                    r.buf_ring.as_mut().unwrap().put(bid);
                }
                false
            };
            if close_now {
                let _ = finish_close(&mut r.ring, &mut r.conns, slot);
            }
            continue;
        }
        if tag == TAG_WRITE && res < 0 {
            if let Some(c) = r.conns.get(&slot) {
                if c.gen == gen {
                    let _ = finish_close(&mut r.ring, &mut r.conns, slot);
                }
            }
            continue;
        }
        if tag == TAG_CLOSE {
            continue;
        }
        let close_now = {
            let Some(c) = r.conns.get_mut(&slot) else {
                continue;
            };
            if c.gen != gen {
                continue;
            }
            match tag {
                TAG_WRITE => {
                    after_write(&mut r.ring, ring_idx, slot, c, app, write_tx, write_result_tx, write_event_fd, feed_reads, post_reads, res as i32)?
                }
                _ => false,
            }
        };
        if close_now {
            let _ = finish_close(&mut r.ring, &mut r.conns, slot);
        }
    }
    r.last_cqes = cqe_count;
    Ok(full)
}

fn submit_rings(rings: &mut [RingState]) {
    for r in rings {
        if !r.ring.submission().is_empty() {
            let _ = r.ring.submit();
        }
    }
}

fn apply_feed_jobs(
    app: &mut App,
    rings: &mut [RingState],
    jobs: &[ReadJob],
    normal: Option<Arc<Vec<u8>>>,
    write_rx: &Receiver<WriteResult>,
) {
    let feed_packet = normal;
    for (i, job) in jobs.iter().enumerate() {
        let Some(r) = rings.get_mut(job.ring) else { continue };
        let Some(c) = r.conns.get_mut(&job.slot) else { continue };
        if c.gen != job.gen {
            continue;
        }
        match &feed_packet {
            Some(normal) => {
                let packet = if job.close {
                    Arc::new(close_packet(normal))
                } else {
                    normal.clone()
                };
                queue_shared(c, packet, job.close);
                stats::record(stats::FEED, job.at);
            }
            None => queue_reply(c, 500, &super::error("internal server error"), true),
        }
        let close_now = c.close_after && c.out.len() == 0;
        if close_now {
            let _ = finish_close(&mut r.ring, &mut r.conns, job.slot);
        } else {
            let _ = arm_write(&mut r.ring, job.slot, c);
        }
        if i % 256 == 255 {
            process_writer(app, rings, write_rx);
        }
    }
}

fn apply_feed_one(rings: &mut [RingState], job: &ReadJob, packet: Option<Vec<u8>>) {
    let Some(r) = rings.get_mut(job.ring) else { return };
    let Some(c) = r.conns.get_mut(&job.slot) else { return };
    if c.gen != job.gen {
        return;
    }
    match packet {
        Some(packet) => {
            let packet = if job.close {
                close_packet(&packet)
            } else {
                packet
            };
            queue_owned(c, packet, job.close);
            stats::record(stats::FEED, job.at);
        }
        None => queue_reply(c, 500, &super::error("internal server error"), true),
    }
    if c.close_after && c.out.len() == 0 {
        let _ = finish_close(&mut r.ring, &mut r.conns, job.slot);
    } else {
        let _ = arm_write(&mut r.ring, job.slot, c);
    }
}

fn apply_post_jobs(
    app: &mut App,
    rings: &mut [RingState],
    jobs: &[ReadJob],
    packets: Option<Arc<Vec<(i64, Option<Arc<PostResponse>>)>>>,
    write_rx: &Receiver<WriteResult>,
) {
    for (i, job) in jobs.iter().enumerate() {
        let Some(r) = rings.get_mut(job.ring) else { continue };
        let Some(c) = r.conns.get_mut(&job.slot) else { continue };
        if c.gen != job.gen {
            continue;
        }
        let id = match job.kind {
            ReadKind::Post(id) => id,
            _ => continue,
        };
        let post = packets
            .as_ref()
            .and_then(|v| v.binary_search_by_key(&id, |e| e.0).ok().and_then(|i| v[i].1.clone()));
        match post {
            Some(post) => {
                let packet = if job.close {
                    Arc::new(close_packet(&post.normal))
                } else {
                    post.normal.clone()
                };
                queue_shared(c, packet, job.close);
                stats::record(stats::POST, job.at);
            }
            None if packets.is_some() => {
                queue_reply(c, 404, &super::error("post not found"), job.close);
                stats::record(stats::POST, job.at);
            }
            None => {
                queue_reply(c, 500, &super::error("internal server error"), true);
                stats::record(stats::POST, job.at);
            }
        }
        let close_now = c.close_after && c.out.len() == 0;
        if close_now {
            let _ = finish_close(&mut r.ring, &mut r.conns, job.slot);
        } else {
            let _ = arm_write(&mut r.ring, job.slot, c);
        }
        if i % 256 == 255 {
            process_writer(app, rings, write_rx);
        }
    }
}

fn apply_post_one(
    rings: &mut [RingState],
    job: &ReadJob,
    packet: Option<Vec<u8>>,
    not_found: bool,
) {
    let Some(r) = rings.get_mut(job.ring) else { return };
    let Some(c) = r.conns.get_mut(&job.slot) else { return };
    if c.gen != job.gen {
        return;
    }
    match packet {
        Some(normal) => {
            let packet = if job.close {
                close_packet(&normal)
            } else {
                normal
            };
            queue_owned(c, packet, job.close);
            stats::record(stats::POST, job.at);
        }
        None if not_found => {
            queue_reply(c, 404, &super::error("post not found"), job.close);
            stats::record(stats::POST, job.at);
        }
        None => {
            queue_reply(c, 500, &super::error("internal server error"), true);
            stats::record(stats::POST, job.at);
        }
    }
    if c.close_after && c.out.len() == 0 {
        let _ = finish_close(&mut r.ring, &mut r.conns, job.slot);
    } else {
        let _ = arm_write(&mut r.ring, job.slot, c);
    }
}

fn process_writer(
    app: &mut App,
    rings: &mut [RingState],
    rx: &Receiver<WriteResult>,
) {
    let mut dirty = 0u64;
    while let Ok(result) = rx.try_recv() {
        dirty |= 1u64 << result.ring;
        if result.feed_dirty {
            app.read.invalidate_feed();
            app.read.invalidate_post();
        }
        if result.like_dirty {
            app.read.invalidate_like();
        }
        let Some(r) = rings.get_mut(result.ring) else {
            continue;
        };
        let close_now = {
            let Some(c) = r.conns.get_mut(&result.slot) else {
                continue;
            };
            if c.gen != result.gen {
                continue;
            }
            c.write_pending = false;
            queue_reply(c, result.status, &result.body, result.close);
            stats::record(result.stat, result.at);
            if c.close_after && c.out.len() == 0 {
                true
            } else {
                let _ = arm_write(&mut r.ring, result.slot, c);
                false
            }
        };
        if close_now {
            let _ = finish_close(&mut r.ring, &mut r.conns, result.slot);
        }
    }
    if dirty != 0 {
        for (i, r) in rings.iter_mut().enumerate() {
            if dirty & (1u64 << i) != 0 {
                let _ = r.ring.submit();
            }
        }
    }
}

fn drain_writes(write_rx: &Receiver<WriteJob>, pending: &mut Vec<WriteJob>) {
    while let Ok(job) = write_rx.try_recv() {
        pending.push(job);
    }
}

fn apply_writes(app: &mut App, rings: &mut [RingState], jobs: &mut Vec<WriteJob>) {
    if jobs.is_empty() {
        return;
    }
    let at = diag_start();
    let (mut results, committed) =
        super::writer::run_batch(&mut app.write, &mut app.write_cache, &jobs);
    diag_end(Diag::Write, at);
    let mut touched = vec![false; rings.len()];
    for (i, job) in jobs.drain(..).enumerate() {
        let pair = results
            .get_mut(i)
            .map(|r| std::mem::replace(r, (0, Vec::new())))
            .filter(|r| r.0 != 0);
        let (status, body) = if committed {
            pair.unwrap_or_else(|| (500, crate::error("internal server error")))
        } else {
            (500, crate::error("internal server error"))
        };
        let close = job.close || !committed;
        let Some(r) = rings.get_mut(job.ring) else { continue };
        touched[job.ring] = true;
        let Some(c) = r.conns.get_mut(&job.slot) else { continue };
        if c.gen != job.gen {
            continue;
        }
        c.write_pending = false;
        queue_reply(c, status, &body, close);
        stats::record(job.stat, job.at);
        if c.close_after && c.out.len() == 0 {
            let _ = finish_close(&mut r.ring, &mut r.conns, job.slot);
        } else {
            let _ = arm_write(&mut r.ring, job.slot, c);
        }
    }
    if committed {
        app.read.invalidate_feed();
        app.read.invalidate_post();
        app.read.invalidate_like();
    }
    for (i, r) in rings.iter_mut().enumerate() {
        if touched[i] {
            let _ = r.ring.submit();
        }
    }
}

fn apply_reads(
    app: &mut App,
    rings: &mut [RingState],
    feed_reads: &mut Vec<ReadJob>,
    post_reads: &mut Vec<ReadJob>,
    write_res_rx: &Receiver<WriteResult>,
) {
    if !feed_reads.is_empty() {
        if app.coalesce {
            let at = diag_start();
            let packet = app.read.feed_response().ok().map(Arc::new);
            diag_end(Diag::Feed, at);
            apply_feed_jobs(app, rings, feed_reads, packet, write_res_rx);
        } else {
            let n = feed_reads.len().min(FEED_ANSWERS_PER_PASS);
            let ok = app.read.begin_read();
            for job in &feed_reads[..n] {
                let at = diag_start();
                let packet = if ok {
                    app.read.feed_response().ok()
                } else {
                    None
                };
                diag_end(Diag::Feed, at);
                apply_feed_one(rings, job, packet);
            }
            app.read.end_read(ok);
            feed_reads.drain(..n);
        }
        if app.coalesce {
            feed_reads.clear();
        }
    }
    if !post_reads.is_empty() {
        if app.coalesce {
            let post_slice = &post_reads[..];
            let mut ids = std::mem::take(&mut app.post_ids);
            ids.clear();
            ids.extend(post_slice.iter().filter_map(|job| match job.kind {
                ReadKind::Post(id) => Some(id),
                _ => None,
            }));
            ids.sort_unstable();
            ids.dedup();
            let at = diag_start();
            let bodies = app.read.posts_sorted(&ids).ok();
            diag_end(Diag::Post, at);
            let packets: Option<Arc<Vec<(i64, Option<Arc<PostResponse>>)>>> = bodies.map(|bodies| {
                Arc::new(
                    bodies
                        .into_iter()
                        .map(|(id, body)| {
                            let response = body.map(|body| {
                                let normal = Arc::new(response_bytes(200, &body, false));
                                Arc::new(PostResponse { normal })
                            });
                            (id, response)
                        })
                        .collect(),
                )
            });
            apply_post_jobs(app, rings, post_slice, packets, write_res_rx);
            ids.clear();
            app.post_ids = ids;
        } else {
            let n = post_reads.len().min(POST_ANSWERS_PER_PASS);
            let ok = app.read.begin_read();
            for job in &post_reads[..n] {
                let id = match job.kind {
                    ReadKind::Post(id) => id,
                    _ => continue,
                };
                if !ok {
                    apply_post_one(rings, job, None, false);
                    continue;
                }
                let at = diag_start();
                let result = app.read.post_response(id);
                diag_end(Diag::Post, at);
                match result {
                    Ok(Some(packet)) => {
                        apply_post_one(rings, job, Some(packet), false)
                    }
                    Ok(None) => apply_post_one(rings, job, None, true),
                    Err(_) => apply_post_one(rings, job, None, false),
                }
            }
            app.read.end_read(ok);
            post_reads.drain(..n);
        }
        if app.coalesce {
            post_reads.clear();
        }
    }
    submit_rings(rings);
}

pub fn run_pass(
    db_path: String,
    secret: Vec<u8>,
    coalesce: bool,
    addr: String,
    ring_count: usize,
) -> io::Result<()> {
    let mut app = App::open(&db_path, secret, coalesce)
        .map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;
    let mut rings = Vec::with_capacity(ring_count.max(1));
    for _ in 0..ring_count.max(1) {
        rings.push(RingState::new(&addr)?);
    }
    let (write_tx, write_rx) = std::sync::mpsc::channel();
    let (res_tx, write_res_rx) = std::sync::mpsc::channel();
    let dirty = std::sync::Arc::new(AtomicU64::new(0));
    let (writer_ready_tx, writer_ready_rx) = std::sync::mpsc::channel();
    crate::writer::spawn(db_path.clone(), write_rx, dirty, writer_ready_tx);
    let _ = writer_ready_rx.recv();
    let mut feed_reads: Vec<ReadJob> = Vec::new();
    let mut post_reads: Vec<ReadJob> = Vec::new();
    let mut fds: Vec<libc::pollfd> = Vec::with_capacity(rings.len());
    let pass_stats = std::env::var("PASS_STATS").is_ok();
    let mut passes = 0u64;
    let mut pass_no = 0u64;
    let mut cqes_total = 0u64;
    let mut proc_ns = 0u128;
    let mut write_ns = 0u128;
    let mut read_ns = 0u128;
    let mut poll_ns = 0u128;
    let mut ring_cursor = 0usize;
    loop {
        let t0 = Instant::now();
        let mut cqes = 0u64;
        let mut queue_full = false;
        let nrings = rings.len();
        for k in 0..nrings {
            let ri = (ring_cursor + k) % nrings;
            if rings[ri].accept_paused
                && rings[ri].accept_resume.is_some_and(|t| Instant::now() >= t)
            {
                rings[ri].accept_paused = false;
                rings[ri].accept_resume = None;
                let _ = rings[ri].arm_accept();
            }
            queue_full |= process_ring(
                ri,
                &mut rings[ri],
                &mut app,
                &write_tx,
                &res_tx,
                -1,
                &mut feed_reads,
                &mut post_reads,
            )?;
            cqes += rings[ri].last_cqes as u64;
        }
        ring_cursor = (ring_cursor + 1) % nrings;
        let t1 = Instant::now();
        process_writer(&mut app, &mut rings, &write_res_rx);
        let t2 = Instant::now();
        let feed_q = feed_reads.len();
        let post_q = post_reads.len();
        let nwrites = 0usize;
        app.read.refresh();
        apply_reads(&mut app, &mut rings, &mut feed_reads, &mut post_reads, &write_res_rx);
        let t3 = Instant::now();
        pass_no += 1;
        ACTIVE.store(
            rings.iter().map(|r| r.conns.len() as u64).sum(),
            Ordering::Relaxed,
        );
        diag_tick(pass_no, feed_q, post_q);

        fds.clear();
        for r in &rings {
            fds.push(libc::pollfd {
                fd: r.ring.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            });
        }
        let t4 = if cqes > 0 || queue_full {
            t3
        } else {
            let rc = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, 1) };
            if rc < 0 {
                let e = io::Error::last_os_error();
                if e.raw_os_error() != Some(libc::EINTR) {
                    return Err(e);
                }
            }
            Instant::now()
        };
        if diag_on() {
            let slow = t4.duration_since(t0);
            if slow.as_millis() >= 20 {
                eprintln!(
                    "slow total_us={} proc_us={} write_us={} read_us={} poll_us={} cqes={} feed_q={} post_q={} writes={}",
                    slow.as_micros(),
                    t1.duration_since(t0).as_micros(),
                    t2.duration_since(t1).as_micros(),
                    t3.duration_since(t2).as_micros(),
                    t4.duration_since(t3).as_micros(),
                    cqes,
                    feed_q,
                    post_q,
                    nwrites
                );
            }
        }

        if pass_stats {
            passes += 1;
            cqes_total += cqes;
            proc_ns += t1.duration_since(t0).as_nanos();
            write_ns += t2.duration_since(t1).as_nanos();
            read_ns += t3.duration_since(t2).as_nanos();
            poll_ns += t4.duration_since(t3).as_nanos();
            if passes % 100 == 0 {
                let n = passes as u128;
                let total = t4.duration_since(t0).as_nanos();
                eprintln!(
                    "pass n={} cqes={} total_ms={} proc_ms={} write_ms={} read_ms={} poll_ms={}",
                    passes,
                    cqes_total / passes,
                    total / 1_000_000,
                    proc_ns / n / 1_000_000,
                    write_ns / n / 1_000_000,
                    read_ns / n / 1_000_000,
                    poll_ns / n / 1_000_000
                );
            }
        }
    }
}
