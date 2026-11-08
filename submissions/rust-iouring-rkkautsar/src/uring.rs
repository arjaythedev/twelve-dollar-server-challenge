use std::alloc::{alloc_zeroed, dealloc, Layout};
use std::collections::VecDeque;
use std::io;
use std::mem::size_of;
use std::net::{SocketAddr, ToSocketAddrs};
use std::os::fd::AsRawFd;
use std::sync::mpsc::{Receiver, Sender};
use std::sync::atomic::{AtomicU16, AtomicU64, AtomicI64, Ordering};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use io_uring::{cqueue, opcode, squeue, types, IoUring};

use super::{parse_request, response_bytes, App, Handled, ParseResult};
use crate::stats;
use crate::writer::{WriteJob, WriteResult};

const TAG_ACCEPT: u64 = 1;
const TAG_READ: u64 = 2;
const TAG_WRITE: u64 = 3;
const TAG_CLOSE: u64 = 4;
const TAG_READ_SINGLE: u64 = 5;
const TAG_POLL: u64 = 6;
const TAG_CANCEL: u64 = 7;
const TAG_SHUTDOWN: u64 = 8;
const TAG_RECV_MULTI: u64 = 9;
const TAG_SHIFT: u64 = 52;
const GEN_SHIFT: u64 = 20;
const SLOT_MASK: u64 = (1 << 20) - 1;

const FILE_TABLE: u32 = 65_000;
const RING_ENTRIES: u32 = 4_096;
const CQES_PER_RING: usize = 4_096;
const READ_QUEUE_CAP: usize = 16_384;
const READ_REARM_PER_PASS: usize = 16_384;
const FEED_ANSWERS_PER_PASS: usize = 1_024;
const POST_ANSWERS_PER_PASS: usize = 1_024;
const READ_BUF: usize = 2_048;
const BUF_READ: usize = 512;
// 2 MiB of provided data buffers per ring; excess readable sockets wait in
// read_queue until a previous receive's completion returns its buffer.
const BUF_RING_ENTRIES: u16 = 4_096;
const BUF_GROUP: u16 = 1;

#[derive(Clone, Copy)]
enum Diag { Process, Feed, Post, Write, Send, Submit }

static DIAG_N: [AtomicU64; 6] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
];
static DIAG_NS: [AtomicU64; 6] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
];
static DIAG_MAX: [AtomicU64; 6] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
];
static ACCEPTS: AtomicU64 = AtomicU64::new(0);
static CLOSES: AtomicU64 = AtomicU64::new(0);
static EOFS: AtomicU64 = AtomicU64::new(0);
static READ_ERRORS: AtomicU64 = AtomicU64::new(0);
static ENOBUFS_COUNT: AtomicU64 = AtomicU64::new(0);
static ACCEPT_ERRORS: AtomicU64 = AtomicU64::new(0);
static ACCEPT_LAST_ERR: AtomicI64 = AtomicI64::new(0);
static ACCEPT_PAUSED: AtomicU64 = AtomicU64::new(0);
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
    for (name, i) in [("proc", 0), ("feed", 1), ("post", 2), ("write", 3), ("send", 4), ("submit", 5)] {
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
        "diag pass={} accepts={} closes={} eofs={} rerr={} enobufs={} aerr={} alast={} apaused={} active={} fq={} pq={} utime_us={} stime_us={}{}",
        passes,
        ACCEPTS.load(Ordering::Relaxed),
        CLOSES.load(Ordering::Relaxed),
        EOFS.load(Ordering::Relaxed),
        READ_ERRORS.load(Ordering::Relaxed),
        ENOBUFS_COUNT.load(Ordering::Relaxed),
        ACCEPT_ERRORS.load(Ordering::Relaxed),
        ACCEPT_LAST_ERR.load(Ordering::Relaxed),
        ACCEPT_PAUSED.load(Ordering::Relaxed),
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
}

impl OutBuf {
    fn len(&self) -> usize {
        match self {
            OutBuf::Empty => 0,
            OutBuf::Owned(v, off) => v.len().saturating_sub(*off),
        }
    }
    fn ptr(&self, off: usize) -> *const u8 {
        match self {
            OutBuf::Empty => std::ptr::null(),
            OutBuf::Owned(v, o) => unsafe { v.as_ptr().add(*o + off) },
        }
    }
    fn advance(&mut self, n: usize) {
        match self {
            OutBuf::Empty => {}
            OutBuf::Owned(_, off) => *off += n,
        }
    }
}


struct Conn {
    input: Vec<u8>,
    read: Option<Box<[u8; READ_BUF]>>,
    out: OutBuf,
    gen: u32,
    poll_read: bool,
    hybrid: bool,
    // Poll notifications are hints; at most one queued or in-flight recv exists.
    poll_armed: bool,
    read_ready: bool,
    peer_hup: bool,
    read_queued: bool,
    reading: bool,
    response_pending: bool,
    closing: bool,
    shutdown_pending: bool,
    writing: bool,
    send_start: Option<Instant>,
    write_pending: bool,
    close_after: bool,
}

impl Conn {
    fn new(gen: u32, poll_read: bool, hybrid: bool) -> Self {
        Self {
            input: Vec::new(),
            read: if poll_read { None } else { Some(Box::new([0u8; READ_BUF])) },
            out: OutBuf::Empty,
            gen,
            poll_read,
            hybrid,
            poll_armed: false,
            read_ready: false,
            peer_hup: false,
            read_queued: false,
            reading: false,
            response_pending: false,
            closing: false,
            shutdown_pending: false,
            writing: false,
            send_start: None,
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
            (&*(br.mem.add(14) as *const AtomicU16)).store(br.tail as u16, Ordering::Release);
        }
        Ok(br)
    }

    fn put(&mut self, bid: u16) {
        if (bid as usize) >= self.bufs.len() / BUF_READ {
            return;
        }
        let slot = (self.tail & self.mask) as usize ;
        // Entry zero's reserved field overlaps the shared tail.
        let entry = unsafe { self.mem.add(slot * 16) as *mut UringBuf };
        unsafe {
            std::ptr::addr_of_mut!((*entry).addr).write(self.bufs.as_ptr().add(bid as usize * BUF_READ) as u64);
            std::ptr::addr_of_mut!((*entry).len).write(BUF_READ as u32);
            std::ptr::addr_of_mut!((*entry).bid).write(bid);
        }
        self.tail = (self.tail.wrapping_add(1)) & 0xffff;
        unsafe {
            (&*(self.mem.add(14) as *const AtomicU16)).store(self.tail as u16, Ordering::Release);
        }
    }
}

impl Drop for BufRing {
    fn drop(&mut self) {
        unsafe { dealloc(self.mem, self.layout) };
    }
}

// Direct descriptors are indices in the registered file table.
struct ConnMap {
    slots: Box<[Option<Conn>]>,
    live: usize,
}

impl ConnMap {
    fn new() -> Self {
        Self {
            slots: std::iter::repeat_with(|| None)
                .take(FILE_TABLE as usize).collect(),
            live: 0,
        }
    }

    fn insert(&mut self, slot: u32, conn: Conn) {
        if self.slots[slot as usize].replace(conn).is_none() {
            self.live += 1;
        }
    }

    fn get(&self, slot: &u32) -> Option<&Conn> {
        self.slots.get(*slot as usize)?.as_ref()
    }

    fn get_mut(&mut self, slot: &u32) -> Option<&mut Conn> {
        self.slots.get_mut(*slot as usize)?.as_mut()
    }

    fn remove(&mut self, slot: &u32) -> Option<Conn> {
        let conn = self.slots.get_mut(*slot as usize)?.take();
        if conn.is_some() {
            self.live -= 1;
        }
        conn
    }

    fn len(&self) -> usize {
        self.live
    }
}

struct RingState {
    ring: IoUring,
    listener: i32,
    conns: ConnMap,
    read_queue: VecDeque<(u32, u32)>,
    reads_in_flight: usize,
    poll_read: bool,
    hybrid: bool,
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
                // Dropping SubmissionQueue publishes the updated tail.
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
        let (poll_read, hybrid) = match std::env::var("RECV_MODE").as_deref() {
            Ok("hybrid") | Err(std::env::VarError::NotPresent) => (true, true),
            Ok("poll") => (true, false),
            Ok("direct") => (false, false),
            _ => return Err(io::Error::new(io::ErrorKind::InvalidInput,
                                          "RECV_MODE must be hybrid, poll, or direct")),
        };
        let listener = make_listener(addr)?;
        let ring = {
            let mut last = None;
            let mut ring = None;
            for attempt in 0..6 {
                let mut builder = IoUring::builder();
                builder.setup_single_issuer().setup_coop_taskrun().setup_cqsize(16_384);
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
        let buf_ring = if poll_read {
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
            conns: ConnMap::new(),
            read_queue: VecDeque::with_capacity(READ_REARM_PER_PASS),
            reads_in_flight: 0,
            poll_read,
            hybrid,
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
    if c.close_after || c.closing {
        return Ok(());
    }
    if c.poll_read {
        if c.poll_armed || (c.hybrid && (c.reading || c.read_ready || c.peer_hup)) {
            return Ok(());
        }
        let entry = if c.hybrid {
            opcode::RecvMulti::new(types::Fixed(slot), BUF_GROUP)
                .build().user_data(ud(slot, c.gen, TAG_RECV_MULTI))
        } else {
            opcode::PollAdd::new(types::Fixed(slot), (libc::POLLIN | libc::POLLRDHUP) as u32)
                .multi(true).build().user_data(ud(slot, c.gen, TAG_POLL))
        };
        push(ring, entry)?;
        c.poll_armed = true;
        Ok(())
    } else {
        let buf = c.read.as_mut().unwrap();
        let entry = opcode::Recv::new(types::Fixed(slot), buf.as_mut_ptr(), READ_BUF as u32)
            .build()
            .user_data(ud(slot, c.gen, TAG_READ));
        push(ring, entry)
    }
}

fn queue_ready(queue: &mut VecDeque<(u32, u32)>, slot: u32, c: &mut Conn) {
    if c.poll_read && c.read_ready && !c.read_queued && !c.reading
        && (!c.hybrid || (!c.poll_armed && !c.peer_hup))
        && !c.response_pending && !c.closing && !c.close_after
    {
        c.read_queued = true;
        queue.push_back((slot, c.gen));
    }
}

fn arm_single_read(ring: &mut IoUring, slot: u32, c: &Conn) -> io::Result<()> {
    if c.close_after {
        return Ok(());
    }
    let entry = opcode::Recv::new(types::Fixed(slot), std::ptr::null_mut(), 0)
        .buf_group(BUF_GROUP)
        .flags(libc::MSG_DONTWAIT)
        .build()
        .flags(squeue::Flags::BUFFER_SELECT)
        .user_data(ud(slot, c.gen, TAG_READ_SINGLE));
    push(ring, entry)
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
    c.send_start = diag_start();
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


fn queue_owned(c: &mut Conn, packet: Vec<u8>, close: bool) {
    c.out = OutBuf::Owned(packet, 0);
    if close {
        c.close_after = true;
    }
}

fn finish_close(ring: &mut IoUring, conns: &mut ConnMap, slot: u32) -> io::Result<()> {
    let Some(c) = conns.get_mut(&slot) else { return Ok(()) };
    if !c.closing {
        c.closing = true;
        CLOSES.fetch_add(1, Ordering::Relaxed);
        if c.poll_read {
            // Cancel by the generation-qualified poll user_data, not by fd:
            // a fixed-file poll retains the socket even after the slot closes.
            if c.poll_armed {
                let tag = if c.hybrid { TAG_RECV_MULTI } else { TAG_POLL };
                let cancel = opcode::AsyncCancel::new(ud(slot, c.gen, tag))
                    .build().user_data(ud(slot, c.gen, TAG_CANCEL));
                push(ring, cancel)?;
            }
            // A send can be pending when the poll fails. Shutdown wakes it;
            // the slot and all borrowed buffers stay alive through its CQE.
            let shutdown = opcode::Shutdown::new(types::Fixed(slot), libc::SHUT_RDWR)
                .build().user_data(ud(slot, c.gen, TAG_SHUTDOWN));
            push(ring, shutdown)?;
            c.shutdown_pending = true;
        }
    }
    if !c.poll_armed && !c.reading && !c.writing && !c.shutdown_pending {
        let gen = c.gen;
        arm_close(ring, slot, gen)?;
        conns.remove(&slot);
    }
    Ok(())
}

// The returned work owns everything it needs; no request borrow escapes this
// function. Only incomplete requests and pipelined bytes need retaining.
fn receive_input(input: &mut Vec<u8>, data: &[u8], app: &mut App) -> Option<(Handled, bool)> {
    if input.is_empty() {
        if let ParseResult::Complete(req, used) = parse_request(data) {
            let close = req.close;
            let handled = app.handle(&req);
            input.extend_from_slice(&data[used..]);
            return Some((handled, close));
        }
    }
    input.extend_from_slice(data);
    None
}

fn process_input(
    ring: &mut IoUring,
    ring_idx: usize,
    slot: u32,
    c: &mut Conn,
    app: &mut App,
    write_tx: &Sender<WriteJob>,
    feed_reads: &mut VecDeque<ReadJob>,
    post_reads: &mut VecDeque<ReadJob>,
    ready: Option<(Handled, bool)>,
) -> io::Result<()> {
    let at = diag_start();
    let r = process_input_inner(
        ring,
        ring_idx,
        slot,
        c,
        app,
        write_tx,
        feed_reads,
        post_reads,
        ready,
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
    feed_reads: &mut VecDeque<ReadJob>,
    post_reads: &mut VecDeque<ReadJob>,
    ready: Option<(Handled, bool)>,
) -> io::Result<()> {
    if c.response_pending || c.writing || c.write_pending || c.close_after || c.closing {
        return Ok(());
    }
    // No next read while a response (including a queued database job) is
    // outstanding. after_write processes buffered pipelined input first.
    let (handled, close) = if let Some(ready) = ready {
        ready
    } else {
        match parse_request(&c.input) {
            ParseResult::Incomplete => return arm_read(ring, slot, c),
            ParseResult::Bad => {
                c.response_pending = true;
                queue_reply(c, 400, br#"{"error":"bad request"}"#, true);
                return arm_write(ring, slot, c);
            }
            ParseResult::Complete(req, used) => {
                let close = req.close;
                let handled = app.handle(&req);
                if used == c.input.len() {
                    c.input.clear();
                } else {
                    c.input.drain(..used);
                }
                if c.input.is_empty() && c.input.capacity() > 64 * 1024 {
                    c.input.shrink_to(4 * 1024);
                }
                (handled, close)
            }
        }
    };
    c.response_pending = true;
    match handled {
                Handled::Reply(status, body) => {
                    queue_reply(c, status, &body, close);
                    arm_write(ring, slot, c)
                }
                Handled::Feed => {
                    feed_reads.push_back(ReadJob {
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
                    post_reads.push_back(ReadJob {
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

fn after_write(
    ring: &mut IoUring,
    ring_idx: usize,
    slot: u32,
    c: &mut Conn,
    app: &mut App,
    write_tx: &Sender<WriteJob>,
    feed_reads: &mut VecDeque<ReadJob>,
    post_reads: &mut VecDeque<ReadJob>,
    res: i32,
) -> io::Result<bool> {
    c.writing = false;
    diag_end(Diag::Send, c.send_start.take());
    c.out.advance(res as usize);
    if c.out.len() == 0 {
        c.out = OutBuf::Empty;
        c.response_pending = false;
        if c.close_after {
            return Ok(true);
        }
        if c.hybrid && c.peer_hup && c.input.is_empty() {
            return Ok(true);
        }
        process_input(ring, ring_idx, slot, c, app, write_tx, feed_reads, post_reads, None)?;
        if c.hybrid && c.peer_hup && !c.response_pending {
            return Ok(true);
        }
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
    feed_reads: &mut VecDeque<ReadJob>,
    post_reads: &mut VecDeque<ReadJob>,
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
                r.conns.insert(slot, Conn::new(gen, r.poll_read, r.hybrid));
                if let Some(c) = r.conns.get_mut(&slot) {
                    arm_read(&mut r.ring, slot, c)?;
                }
            } else {
                r.accepting = false;
                let errno = -res;
                ACCEPT_ERRORS.fetch_add(1, Ordering::Relaxed);
                ACCEPT_LAST_ERR.store(errno as i64, Ordering::Relaxed);
                if errno == libc::ENFILE || errno == libc::EMFILE || errno == libc::ENOMEM {
                    ACCEPT_PAUSED.fetch_add(1, Ordering::Relaxed);
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
        if tag == TAG_CLOSE {
            if res < 0 {
                return Err(io::Error::from_raw_os_error(-res));
            }
            continue;
        }
        if tag == TAG_CANCEL {
            if res == -libc::EALREADY {
                if let Some(c) = r.conns.get(&slot) {
                    if c.gen == gen && c.closing && c.poll_armed {
                        let tag = if c.hybrid { TAG_RECV_MULTI } else { TAG_POLL };
                        let cancel = opcode::AsyncCancel::new(ud(slot, gen, tag))
                            .build().user_data(ud(slot, gen, TAG_CANCEL));
                        push(&mut r.ring, cancel)?;
                    }
                }
            } else if res < 0 && res != -libc::ENOENT {
                return Err(io::Error::from_raw_os_error(-res));
            }
            continue;
        }
        if tag == TAG_SHUTDOWN {
            // An already disconnected TCP socket may report ENOTCONN; its
            // outstanding socket operations will still complete with errors.
            if res < 0 && res != -libc::ENOTCONN {
                return Err(io::Error::from_raw_os_error(-res));
            }
            if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen == gen {
                    c.shutdown_pending = false;
                    finish_close(&mut r.ring, &mut r.conns, slot)?;
                }
            }
            continue;
        }
        if tag == TAG_RECV_MULTI {
            let bid = cqueue::buffer_select(flags);
            let mut ready = None;
            let mut close_now = false;
            if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen == gen {
                    if !cqueue::more(flags) {
                        c.poll_armed = false;
                    }
                    if c.closing {
                        close_now = true;
                    } else if res > 0 {
                        match bid {
                            Some(bid) if (bid as usize) < BUF_RING_ENTRIES as usize
                                && (res as usize) <= BUF_READ
                                && c.input.len() + (res as usize) <= (1 << 20) => {
                                let start = bid as usize * BUF_READ;
                                let data = &r.buf_ring.as_ref().unwrap().bufs[start..start + res as usize];
                                if c.response_pending || c.close_after {
                                    if !c.close_after {
                                        c.input.extend_from_slice(data);
                                    }
                                } else {
                                    ready = receive_input(&mut c.input, data, app);
                                }
                                if !c.poll_armed {
                                    c.read_ready = true;
                                }
                            }
                            _ => close_now = true,
                        }
                    } else if res == 0 {
                        EOFS.fetch_add(1, Ordering::Relaxed);
                        c.peer_hup = true;
                        close_now = !c.response_pending;
                    } else if res == -libc::ENOBUFS || res == -libc::EINTR || res == -libc::EAGAIN {
                        if res == -libc::ENOBUFS {
                            ENOBUFS_COUNT.fetch_add(1, Ordering::Relaxed);
                        }
                        if !c.poll_armed {
                            c.read_ready = true;
                        }
                    } else {
                        READ_ERRORS.fetch_add(1, Ordering::Relaxed);
                        close_now = true;
                    }
                }
            }
            if let Some(bid) = bid {
                r.buf_ring.as_mut().unwrap().put(bid);
            }
            if close_now {
                finish_close(&mut r.ring, &mut r.conns, slot)?;
            } else if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen == gen && !c.closing {
                    if res > 0 {
                        process_input(
                            &mut r.ring, ring_idx, slot, c, app, write_tx,
                            feed_reads, post_reads, ready,
                        )?;
                    }
                    queue_ready(&mut r.read_queue, slot, c);
                }
            }
            continue;
        }
        if tag == TAG_POLL {
            let Some(c) = r.conns.get_mut(&slot) else { continue };
            if c.gen != gen {
                continue;
            }
            if !cqueue::more(flags) {
                c.poll_armed = false;
            }
            if c.closing {
                finish_close(&mut r.ring, &mut r.conns, slot)?;
            } else if res >= 0 {
                c.read_ready = true;
                c.peer_hup |= res & (libc::POLLRDHUP | libc::POLLHUP | libc::POLLERR) as i32 != 0;
                if !c.poll_armed {
                    arm_read(&mut r.ring, slot, c)?;
                }
                queue_ready(&mut r.read_queue, slot, c);
            } else if res == -libc::EINTR {
                if !c.poll_armed {
                    arm_read(&mut r.ring, slot, c)?;
                }
            } else {
                READ_ERRORS.fetch_add(1, Ordering::Relaxed);
                finish_close(&mut r.ring, &mut r.conns, slot)?;
            }
            continue;
        }
        if tag == TAG_READ_SINGLE {
            r.reads_in_flight -= 1;
            let bid = cqueue::buffer_select(flags);
            let mut ready = None;
            let mut close_now = false;
            if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen == gen {
                    c.reading = false;
                    if c.closing {
                        close_now = true;
                    } else if res > 0 {
                        match bid {
                            Some(bid) if (bid as usize) < BUF_RING_ENTRIES as usize
                                && (res as usize) <= BUF_READ
                                && c.input.len() + (res as usize) <= (1 << 20) => {
                                let start = bid as usize * BUF_READ;
                                let data = &r.buf_ring.as_ref().unwrap().bufs[start..start + res as usize];
                                ready = receive_input(&mut c.input, data, app);
                                // A full buffer may leave bytes unread. A hangup
                                // hint requires another read to observe EOF.
                                c.read_ready |= res as usize == BUF_READ || c.peer_hup;
                            }
                            _ => close_now = true,
                        }
                    } else if res == -libc::ENOBUFS || res == -libc::EINTR {
                        if res == -libc::ENOBUFS {
                            ENOBUFS_COUNT.fetch_add(1, Ordering::Relaxed);
                        }
                        c.read_ready = true;
                    } else if res != -libc::EAGAIN {
                        if res == 0 { EOFS.fetch_add(1, Ordering::Relaxed); }
                        else { READ_ERRORS.fetch_add(1, Ordering::Relaxed); }
                        close_now = true;
                    }
                }
            }
            // Return selected buffers even for stale, failed, or closing reads.
            if let Some(bid) = bid {
                r.buf_ring.as_mut().unwrap().put(bid);
            }
            if close_now {
                finish_close(&mut r.ring, &mut r.conns, slot)?;
            } else if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen == gen && !c.closing {
                    if res > 0 {
                        process_input(
                            &mut r.ring, ring_idx, slot, c, app, write_tx,
                            feed_reads, post_reads, ready,
                        )?;
                    }
                    if c.hybrid && !c.read_ready {
                        arm_read(&mut r.ring, slot, c)?;
                    }
                    queue_ready(&mut r.read_queue, slot, c);
                }
            }
            continue;
        }
        if tag == TAG_READ {
            let close_now = if let Some(c) = r.conns.get_mut(&slot) {
                if c.gen != gen {
                    false
                } else if res == -libc::EINTR {
                    arm_read(&mut r.ring, slot, c)?;
                    false
                } else if res <= 0 {
                    true
                } else {
                    let n = res as usize;
                    if n > READ_BUF || c.input.len() + n > (1 << 20) {
                        true
                    } else {
                        let data = &c.read.as_ref().unwrap()[..n];
                        let ready = if !c.writing && !c.write_pending && !c.close_after {
                            receive_input(&mut c.input, data, app)
                        } else {
                            c.input.extend_from_slice(data);
                            None
                        };
                        process_input(
                            &mut r.ring, ring_idx, slot, c, app, write_tx,
                            feed_reads, post_reads, ready,
                        )?;
                        false
                    }
                }
            } else {
                false
            };
            if close_now {
                finish_close(&mut r.ring, &mut r.conns, slot)?;
            }
            continue;
        }
        if tag == TAG_WRITE {
            let Some(c) = r.conns.get_mut(&slot) else { continue };
            if c.gen != gen {
                continue;
            }
            if c.closing || res <= 0 {
                c.writing = false;
                diag_end(Diag::Send, c.send_start.take());
                finish_close(&mut r.ring, &mut r.conns, slot)?;
            } else {
                let close_now = after_write(
                    &mut r.ring, ring_idx, slot, c, app, write_tx,
                    feed_reads, post_reads, res,
                )?;
                if close_now {
                    finish_close(&mut r.ring, &mut r.conns, slot)?;
                } else {
                    queue_ready(&mut r.read_queue, slot, c);
                }
            }
        }
    }
    r.last_cqes = cqe_count;
    for _ in 0..READ_REARM_PER_PASS {
        // Each selected receive can consume one provided buffer until its CQE
        // is reaped. Bound the number in flight rather than overrun the ring.
        if r.reads_in_flight >= BUF_RING_ENTRIES as usize {
            break;
        }
        let Some((slot, gen)) = r.read_queue.pop_front() else { break };
        let Some(c) = r.conns.get_mut(&slot) else { continue };
        if c.gen != gen {
            continue;
        }
        c.read_queued = false;
        if c.closing || c.close_after || c.response_pending || c.reading {
            continue;
        }
        if arm_single_read(&mut r.ring, slot, c).is_err() {
            r.read_queue.push_back((slot, gen));
            c.read_queued = true;
            break;
        }
        c.read_ready = false;
        c.reading = true;
        r.reads_in_flight += 1;
    }
    Ok(full)
}

fn submit_rings(rings: &mut [RingState]) {
    for r in rings {
        if !r.ring.submission().is_empty() {
            let at = diag_start();
            let _ = r.ring.submit();
            diag_end(Diag::Submit, at);
        }
    }
}


fn apply_feed_one(rings: &mut [RingState], job: &ReadJob, packet: Option<Vec<u8>>) {
    let Some(r) = rings.get_mut(job.ring) else { return };
    let Some(c) = r.conns.get_mut(&job.slot) else { return };
    if c.gen != job.gen || c.closing {
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


fn apply_post_one(
    rings: &mut [RingState],
    job: &ReadJob,
    packet: Option<Vec<u8>>,
    not_found: bool,
) {
    let Some(r) = rings.get_mut(job.ring) else { return };
    let Some(c) = r.conns.get_mut(&job.slot) else { return };
    if c.gen != job.gen || c.closing {
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
    while let Ok(result) = rx.try_recv() {
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
            if c.gen != result.gen || c.closing {
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
}



fn apply_reads(
    app: &mut App,
    rings: &mut [RingState],
    feed_reads: &mut VecDeque<ReadJob>,
    post_reads: &mut VecDeque<ReadJob>,
) {
    if !feed_reads.is_empty() {
        let n = feed_reads.len().min(FEED_ANSWERS_PER_PASS);
        let ok = app.read.begin_read();
        for job in feed_reads.iter().take(n) {
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
    if !post_reads.is_empty() {
        let n = post_reads.len().min(POST_ANSWERS_PER_PASS);
        let ok = app.read.begin_read();
        for job in post_reads.iter().take(n) {
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
                Ok(Some(packet)) => apply_post_one(rings, job, Some(packet), false),
                Ok(None) => apply_post_one(rings, job, None, true),
                Err(_) => apply_post_one(rings, job, None, false),
            }
        }
        app.read.end_read(ok);
        post_reads.drain(..n);
    }
    submit_rings(rings);
}

pub fn run_pass(
    db_path: String,
    secret: Vec<u8>,
    addr: String,
    ring_count: usize,
) -> io::Result<()> {
    let mut app = App::open(&db_path, secret)
        .map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;
    let mut rings = Vec::with_capacity(ring_count.max(1));
    for _ in 0..ring_count.max(1) {
        rings.push(RingState::new(&addr)?);
    }
    let (write_tx, write_rx) = std::sync::mpsc::channel();
    let (res_tx, write_res_rx) = std::sync::mpsc::channel();
    let (writer_ready_tx, writer_ready_rx) = std::sync::mpsc::channel();
    crate::writer::spawn(db_path.clone(), write_rx, res_tx, writer_ready_tx);
    let _ = writer_ready_rx.recv();
    let mut feed_reads: VecDeque<ReadJob> = VecDeque::new();
    let mut post_reads: VecDeque<ReadJob> = VecDeque::new();
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
        apply_reads(&mut app, &mut rings, &mut feed_reads, &mut post_reads);
        let t3 = Instant::now();
        pass_no += 1;
        ACTIVE.store(
            rings.iter().map(|r| r.conns.len() as u64).sum(),
            Ordering::Relaxed,
        );
        diag_tick(pass_no, feed_q, post_q);

        let ready_reads = rings.iter().any(|r| {
            !r.read_queue.is_empty() && r.reads_in_flight < BUF_RING_ENTRIES as usize
        });
        // Database work is already runnable even if the rings have no CQEs.
        let t4 = if cqes > 0 || queue_full || ready_reads
            || !feed_reads.is_empty() || !post_reads.is_empty()
        {
            t3
        } else {
            fds.clear();
            for r in &rings {
                fds.push(libc::pollfd {
                    fd: r.ring.as_raw_fd(),
                    events: libc::POLLIN,
                    revents: 0,
                });
            }
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
