use std::alloc::{alloc_zeroed, dealloc, Layout};
use std::collections::HashMap;
use std::io;
use std::mem::size_of;
use std::net::{SocketAddr, ToSocketAddrs};
use std::os::fd::{AsRawFd, RawFd};
use std::sync::mpsc::{Receiver, Sender};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;

use io_uring::{cqueue, opcode, squeue, types, IoUring};

use super::{parse_request, response_bytes, App, Handled, ParseResult};
use crate::reader::{FeedReply, FeedRequest, PostReply, PostRequest, PostResponse};
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
const NEW_RING_AT: usize = 65_000;
const MAX_RINGS: usize = 16;
const MAX_READ_BATCH: usize = 8192;
const READ_BUF: usize = 2_048;
const BUF_READ: usize = 1_024;
const BUF_RING_ENTRIES: u16 = 32_768;
const BUF_GROUP: u16 = 1;

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

struct FeedPending {
    jobs: Vec<ReadJob>,
    rx: Receiver<FeedReply>,
}

struct PostPending {
    jobs: Vec<ReadJob>,
    rx: Receiver<PostReply>,
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
        let slot = ((self.tail & self.mask) as usize);
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
    cqes: Vec<(u64, i32, u32)>,
    multishot: bool,
    buf_ring: Option<BufRing>,
    next_gen: u32,
    accepting: bool,
    accept_paused: bool,
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
    if unsafe { libc::listen(fd, 4096) } < 0 {
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
        let mut builder = IoUring::builder();
        builder.setup_cqsize(8_192);
        let mut ring = match builder.build(RING_ENTRIES) {
            Ok(r) => r,
            Err(e) => {
                unsafe {
                    libc::close(listener);
                }
                return Err(e);
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
            cqes: Vec::with_capacity(256),
            multishot,
            buf_ring,
            next_gen: 1,
            accepting: false,
            accept_paused: false,
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

fn finish_close(ring: &mut IoUring, conns: &mut HashMap<u32, Conn>, slot: u32) -> io::Result<()> {
    if let Some(c) = conns.remove(&slot) {
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
) -> io::Result<()> {
    let mut cqes = std::mem::take(&mut r.cqes);
    cqes.clear();
    for cqe in r.ring.completion() {
        cqes.push((cqe.user_data(), cqe.result(), cqe.flags()));
        if cqes.len() >= 1024 {
            break;
        }
    }
    for (user, res, flags) in cqes.drain(..) {
        if user == TAG_ACCEPT {
            if res >= 0 {
                let slot = res as u32;
                let gen = r.next_gen;
                r.next_gen = r.next_gen.wrapping_add(1).max(1);
                r.conns.insert(slot, Conn::new(gen, r.multishot));
                if let Some(c) = r.conns.get_mut(&slot) {
                    arm_read(&mut r.ring, slot, c)?;
                }
            } else {
                r.accepting = false;
                let errno = -res;
                if errno == libc::ENFILE || errno == libc::EMFILE || errno == libc::ENOMEM {
                    r.accept_paused = true;
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
    r.cqes = cqes;
    r.ring.submit()?;
    Ok(())
}

fn flush_feed(
    reads: &mut Vec<ReadJob>,
    pending: &mut Option<FeedPending>,
    tx: &Sender<FeedRequest>,
    event_fd: RawFd,
) {
    if pending.is_some() || reads.is_empty() {
        return;
    }
    let take = reads.len().min(MAX_READ_BATCH);
    let jobs: Vec<ReadJob> = reads.drain(..take).collect();
    let (reply, rx) = std::sync::mpsc::channel();
    if tx.send(FeedRequest { jobs: jobs.clone(), reply, event_fd }).is_ok() {
        *pending = Some(FeedPending { jobs, rx });
    }
}

fn flush_post(
    reads: &mut Vec<ReadJob>,
    pending: &mut Option<PostPending>,
    tx: &Sender<PostRequest>,
    event_fd: RawFd,
) {
    if pending.is_some() || reads.is_empty() {
        return;
    }
    let take = reads.len().min(MAX_READ_BATCH);
    let jobs: Vec<ReadJob> = reads.drain(..take).collect();
    let (reply, rx) = std::sync::mpsc::channel();
    if tx.send(PostRequest { jobs: jobs.clone(), reply, event_fd }).is_ok() {
        *pending = Some(PostPending { jobs, rx });
    }
}

fn process_feed(
    app: &mut App,
    rings: &mut [RingState],
    pending: &mut Option<FeedPending>,
    write_rx: &Receiver<WriteResult>,
) {
    loop {
        let reply = match pending.as_ref().and_then(|p| p.rx.try_recv().ok()) {
            Some(reply) => reply,
            None => break,
        };
        let Some(p) = pending.take() else { break };
        let feed_packets = reply.body.as_ref().map(|body| {
            (
                Arc::new(response_bytes(200, body, false)),
                Arc::new(response_bytes(200, body, true)),
            )
        });
        let mut dirty = 0u64;
        for (i, job) in p.jobs.iter().enumerate() {
            let Some(r) = rings.get_mut(job.ring) else {
                continue;
            };
            let Some(c) = r.conns.get_mut(&job.slot) else {
                continue;
            };
            if c.gen != job.gen {
                continue;
            }
            match &feed_packets {
                Some((normal, close_packet)) => {
                    let packet = if job.close { close_packet.clone() } else { normal.clone() };
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
                dirty |= 1u64 << job.ring;
            }
            if i % 256 == 255 {
                process_writer(app, rings, write_rx);
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
}

fn process_post(
    app: &mut App,
    rings: &mut [RingState],
    pending: &mut Option<PostPending>,
    write_rx: &Receiver<WriteResult>,
) {
    loop {
        let reply = match pending.as_ref().and_then(|p| p.rx.try_recv().ok()) {
            Some(reply) => reply,
            None => break,
        };
        let Some(p) = pending.take() else { break };
        let mut dirty = 0u64;
        for (i, job) in p.jobs.iter().enumerate() {
            let Some(r) = rings.get_mut(job.ring) else {
                continue;
            };
            let Some(c) = r.conns.get_mut(&job.slot) else {
                continue;
            };
            if c.gen != job.gen {
                continue;
            }
            let id = match job.kind {
                ReadKind::Post(id) => id,
                _ => continue,
            };
            let post = reply
                .packets
                .as_ref()
                .and_then(|map| map.get(&id).cloned())
                .flatten();
            match post {
                Some(post) => {
                    let packet = if job.close {
                        post.close.as_ref().unwrap_or(&post.normal).clone()
                    } else {
                        post.normal.clone()
                    };
                    queue_shared(c, packet, job.close);
                    stats::record(stats::POST, job.at);
                }
                None if reply.packets.is_some() => {
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
                dirty |= 1u64 << job.ring;
            }
            if i % 256 == 255 {
                process_writer(app, rings, write_rx);
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

pub fn run_shard(
    db_path: String,
    secret: Vec<u8>,
    coalesce: bool,
    addr: String,
    write_tx: Sender<WriteJob>,
    write_result_tx: Sender<WriteResult>,
    write_res_rx: Receiver<WriteResult>,
    write_event_fd: RawFd,
    feed_tx: Sender<FeedRequest>,
    post_tx: Sender<PostRequest>,
    dirty: Arc<AtomicU64>,
    shard: usize,
) -> io::Result<()> {
    let mut app = App::open(&db_path, secret, coalesce)
        .map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;
    let mut ring = RingState::new(&addr)?;
    let read_event_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if read_event_fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let _ = shard;
    let mut feed_reads: Vec<ReadJob> = Vec::new();
    let mut post_reads: Vec<ReadJob> = Vec::new();
    let mut feed_pending: Option<FeedPending> = None;
    let mut post_pending: Option<PostPending> = None;
    let mut fds: Vec<libc::pollfd> = Vec::with_capacity(3);
    loop {
        process_writer(&mut app, std::slice::from_mut(&mut ring), &write_res_rx);
        process_feed(
            &mut app,
            std::slice::from_mut(&mut ring),
            &mut feed_pending,
            &write_res_rx,
        );
        process_post(
            &mut app,
            std::slice::from_mut(&mut ring),
            &mut post_pending,
            &write_res_rx,
        );
        app.read.refresh();
        process_ring(
            0,
            &mut ring,
            &mut app,
            &write_tx,
            &write_result_tx,
            write_event_fd,
            &mut feed_reads,
            &mut post_reads,
        )?;
        process_writer(&mut app, std::slice::from_mut(&mut ring), &write_res_rx);
        flush_feed(&mut feed_reads, &mut feed_pending, &feed_tx, read_event_fd);
        flush_post(&mut post_reads, &mut post_pending, &post_tx, read_event_fd);

        fds.clear();
        fds.push(libc::pollfd {
            fd: ring.ring.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        });
        fds.push(libc::pollfd {
            fd: write_event_fd,
            events: libc::POLLIN,
            revents: 0,
        });
        fds.push(libc::pollfd {
            fd: read_event_fd,
            events: libc::POLLIN,
            revents: 0,
        });
        let rc = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, 1) };
        if rc < 0 {
            let e = io::Error::last_os_error();
            if e.raw_os_error() != Some(libc::EINTR) {
                return Err(e);
            }
        }
        let mut val = 0u64;
        unsafe {
            libc::read(
                write_event_fd,
                &mut val as *mut u64 as *mut libc::c_void,
                size_of::<u64>(),
            );
            libc::read(
                read_event_fd,
                &mut val as *mut u64 as *mut libc::c_void,
                size_of::<u64>(),
            );
        }
        process_writer(&mut app, std::slice::from_mut(&mut ring), &write_res_rx);
        process_feed(
            &mut app,
            std::slice::from_mut(&mut ring),
            &mut feed_pending,
            &write_res_rx,
        );
        process_post(
            &mut app,
            std::slice::from_mut(&mut ring),
            &mut post_pending,
            &write_res_rx,
        );
        flush_feed(&mut feed_reads, &mut feed_pending, &feed_tx, read_event_fd);
        flush_post(&mut post_reads, &mut post_pending, &post_tx, read_event_fd);
    }
}
