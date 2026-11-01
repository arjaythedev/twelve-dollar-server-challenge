//! One thread, io_uring, SQLite. See README.md for the design.
mod app;
mod db;
mod http;
mod json;
mod jwt;

use app::App;
use io_uring::{cqueue, opcode, squeue, types, IoUring, SubmissionQueue, Submitter};
use std::io;
use std::os::fd::{AsRawFd, RawFd};
use std::sync::atomic::{AtomicU16, Ordering};

const GETEVENTS: u32 = 1; // IORING_ENTER_GETEVENTS
const OP_ACCEPT: u64 = 1;
const OP_RECV: u64 = 2;
const OP_SEND: u64 = 3;
const OP_SHUTDOWN: u64 = 4;
const OP_CLOSE: u64 = 5;
/// Single-shot accepts kept in flight on the accepting ring, spread evenly over the listeners.
const ACCEPT_DEPTH: u32 = 64;
/// SO_REUSEPORT listeners on the one port. Each has its own accept queue, capped at
/// net.core.somaxconn (4,096 by default), so eight hold eight times as many pending connections.
const LISTENERS: usize = 8;

const SQ_ENTRIES: u32 = 4096;
const CQ_ENTRIES: u32 = 65536;
const BUF_SIZE: usize = 2048;
const BUF_COUNT: u16 = 1024;
const MAX_RINGS: usize = 16;
/// Each pass waits on ring 0 for this many completions or WAIT_NS, whichever comes first.
/// Larger passes share one COMMIT. Newer kernels cap the deferred task work one enter runs
/// in proportion to min_complete; with min_complete = 1 a pass then sees only ~40 receives.
const WAIT_MIN: usize = 256;
const WAIT_NS: u32 = 1_000_000;
/// Output buffers kept for reuse; idle connections hold none.
const POOL_MAX: usize = 2048;
const OUT_CAP: usize = 8192;

#[inline]
fn ud(op: u64, ring: usize, gen: u16, slot: u32) -> u64 {
    op << 56 | (ring as u64) << 48 | (gen as u64) << 32 | slot as u64
}

/// A provided-buffer ring: receives pick a buffer only when data arrives, so idle
/// connections hold no receive memory.
struct BufRing {
    ring: *mut u8,
    tail: u16,
    mem: Vec<u8>,
}

/// Registers buffer group 0. Ubuntu's 6.8.0-136 and later kernels invert the check on the
/// reserved words (Launchpad #2162843): they refuse all zeroes and accept a nonzero `resv[0]`,
/// so a refused registration is tried once more that way. A correct kernel never gets there.
fn register_buf_ring(ring_fd: RawFd, addr: u64) -> io::Result<()> {
    #[repr(C)]
    struct BufReg {
        ring_addr: u64,
        ring_entries: u32,
        bgid: u16,
        flags: u16,
        resv: [u64; 3],
    }
    const IORING_REGISTER_PBUF_RING: libc::c_long = 22;
    let mut reg = BufReg { ring_addr: addr, ring_entries: BUF_COUNT as u32, bgid: 0, flags: 0, resv: [0; 3] };
    let mut rc = 0;
    for _ in 0..2 {
        rc = unsafe { libc::syscall(libc::SYS_io_uring_register, ring_fd, IORING_REGISTER_PBUF_RING, &reg as *const BufReg, 1) };
        if rc == 0 || io::Error::last_os_error().raw_os_error() != Some(libc::EINVAL) {
            break;
        }
        reg.resv[0] = 1;
    }
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

impl BufRing {
    fn new(ring_fd: RawFd) -> io::Result<BufRing> {
        let bytes = BUF_COUNT as usize * 16;
        let ring = unsafe {
            libc::mmap(std::ptr::null_mut(), bytes, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_PRIVATE | libc::MAP_ANONYMOUS, -1, 0)
        };
        if ring == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        let mut b = BufRing { ring: ring as *mut u8, tail: 0, mem: vec![0u8; BUF_COUNT as usize * BUF_SIZE] };
        register_buf_ring(ring_fd, ring as u64)?;
        for i in 0..BUF_COUNT {
            b.add(i);
        }
        b.publish();
        Ok(b)
    }
    #[inline]
    fn add(&mut self, bid: u16) {
        let e = unsafe { self.ring.add((self.tail & (BUF_COUNT - 1)) as usize * 16) };
        unsafe {
            (e as *mut u64).write(self.mem.as_ptr().add(bid as usize * BUF_SIZE) as u64);
            (e.add(8) as *mut u32).write(BUF_SIZE as u32);
            (e.add(12) as *mut u16).write(bid);
        }
        self.tail = self.tail.wrapping_add(1);
    }
    #[inline]
    fn publish(&self) {
        unsafe { (*(self.ring.add(14) as *const AtomicU16)).store(self.tail, Ordering::Release) };
    }
    #[inline]
    fn get(&self, bid: u16, len: usize) -> &[u8] {
        &self.mem[bid as usize * BUF_SIZE..bid as usize * BUF_SIZE + len]
    }
}

#[derive(Default)]
struct Conn {
    gen: u16,
    open: bool,
    /// The connection ends once its output drains.
    close_after: bool,
    /// shutdown/close submitted.
    closing: bool,
    sending: bool,
    in_batch: bool,
    /// Produced a response in this batch (not just marked to flush older output).
    answered: bool,
    batch_mark: u32,
    sent: u32,
    /// Responses not yet handed to the kernel.
    out: Buf,
    /// The one send in flight.
    wire: Buf,
    /// An incomplete request.
    partial: Buf,
}

const _: () = assert!(std::mem::size_of::<Conn>() == 40);

/// A connection's byte buffer: one null pointer while unused, so an idle connection's record
/// stays at 40 bytes (there is one per descriptor slot, in use or not).
#[derive(Default)]
struct Buf(Option<Box<Vec<u8>>>);

impl Buf {
    #[inline]
    fn len(&self) -> usize {
        self.0.as_ref().map_or(0, |v| v.len())
    }
    #[inline]
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
    #[inline]
    fn bytes(&self) -> &[u8] {
        self.0.as_deref().map_or(&[][..], |v| &v[..])
    }
    /// The buffer, taking one from the pool if there is none yet.
    #[inline]
    fn get(&mut self, pool: &mut Vec<Box<Vec<u8>>>) -> &mut Vec<u8> {
        self.0.get_or_insert_with(|| take_buf(pool))
    }
}

struct Ring {
    uring: IoUring,
    bufs: BufRing,
    conns: Vec<Conn>,
    /// Accepts are kept armed here.
    accepting: bool,
    /// Accepts in flight, per listener.
    accepts: [u32; LISTENERS],
    /// Not accepting here: the table is (nearly) full.
    full: bool,
    open: u32,
    /// Accepting moves on while this many slots are still free (more than ACCEPT_DEPTH): an
    /// accept that finds the table full has already taken the connection off the listen queue
    /// and would drop it. Single-shot accepts bound how many can land after the switch.
    margin: u32,
    rearm: Vec<u32>,
}

impl Ring {
    fn new(slots: u32) -> io::Result<Ring> {
        let uring = IoUring::builder()
            .setup_single_issuer()
            .setup_defer_taskrun()
            .setup_submit_all()
            .setup_cqsize(CQ_ENTRIES)
            .build(SQ_ENTRIES)?;
        uring.submitter().register_files_sparse(slots)?;
        let bufs = BufRing::new(uring.as_raw_fd())?;
        let conns = (0..slots).map(|_| Conn::default()).collect();
        // At most a quarter of a small table, so a full ring can still drain below 2x margin.
        let margin = (slots / 8).clamp(ACCEPT_DEPTH + 1, 512).min(slots / 4);
        Ok(Ring { uring, bufs, conns, accepting: false, accepts: [0; LISTENERS], full: false, open: 0, margin, rearm: Vec::new() })
    }
}

/// Queues `e`, first making room for `room` entries (a linked pair must not be split across
/// submissions). If the kernel can't take the queue yet (its completion queue is overflowing),
/// it flushes completions and tries again; only if that keeps failing is the entry dropped,
/// rather than stalling every connection.
#[inline]
fn push(sq: &mut SubmissionQueue, sub: &Submitter, e: &squeue::Entry, room: usize) {
    let mut tries = 0;
    while sq.capacity() - sq.len() < room {
        sq.sync();
        if sub.submit().is_err() {
            let _ = unsafe { sub.enter::<libc::sigset_t>(0, 0, GETEVENTS, None) };
        }
        sq.sync();
        tries += 1;
        if tries == 1000 {
            eprintln!("submission queue stuck full; dropping an entry");
            return;
        }
    }
    unsafe { sq.push(e).expect("submission queue has room") };
}

fn arm_recv(sq: &mut SubmissionQueue, sub: &Submitter, ri: usize, slot: u32, gen: u16) {
    let e = opcode::RecvMulti::new(types::Fixed(slot), 0).build().user_data(ud(OP_RECV, ri, gen, slot));
    push(sq, sub, &e, 1);
}

fn arm_send(sq: &mut SubmissionQueue, sub: &Submitter, ri: usize, slot: u32, c: &Conn) {
    let rest = &c.wire.bytes()[c.sent as usize..];
    let e = opcode::Send::new(types::Fixed(slot), rest.as_ptr(), rest.len() as u32)
        .flags(libc::MSG_NOSIGNAL)
        .build()
        .user_data(ud(OP_SEND, ri, c.gen, slot));
    push(sq, sub, &e, 1);
}

fn arm_close(sq: &mut SubmissionQueue, sub: &Submitter, ri: usize, slot: u32, c: &mut Conn) {
    c.closing = true;
    // The shutdown ends the multishot receive; the close frees the slot.
    let s = opcode::Shutdown::new(types::Fixed(slot), libc::SHUT_RDWR)
        .build()
        .flags(squeue::Flags::IO_HARDLINK)
        .user_data(ud(OP_SHUTDOWN, ri, c.gen, slot));
    let cl = opcode::Close::new(types::Fixed(slot)).build().user_data(ud(OP_CLOSE, ri, c.gen, slot));
    push(sq, sub, &s, 2);
    push(sq, sub, &cl, 1);
}

fn fill_accepts(sq: &mut SubmissionQueue, sub: &Submitter, ri: usize, listen: &[RawFd], accepts: &mut [u32; LISTENERS]) {
    for (li, &fd) in listen.iter().enumerate() {
        while accepts[li] < ACCEPT_DEPTH / LISTENERS as u32 {
            let e = opcode::Accept::new(types::Fd(fd), std::ptr::null_mut(), std::ptr::null_mut())
                .file_index(Some(types::DestinationSlot::auto_target()))
                .build()
                .user_data(ud(OP_ACCEPT, ri, 0, li as u32));
            push(sq, sub, &e, 1);
            accepts[li] += 1;
        }
    }
}

fn arm_accept(r: &mut Ring, ri: usize, listen: &[RawFd]) {
    r.accepting = true;
    let (sub, mut sq, _) = r.uring.split();
    fill_accepts(&mut sq, &sub, ri, listen, &mut r.accepts);
}

struct Server {
    rings: Vec<Ring>,
    app: App,
    listen: Vec<RawFd>,
    slots: u32,
    pool: Vec<Box<Vec<u8>>>,
    touched: Vec<(u8, u32)>,
    need_ring: bool,
    /// When adding a ring failed: don't retry before this.
    spill_after: std::time::Instant,
}

#[inline]
fn take_buf(pool: &mut Vec<Box<Vec<u8>>>) -> Box<Vec<u8>> {
    pool.pop().unwrap_or_else(|| Box::new(Vec::with_capacity(OUT_CAP)))
}

#[inline]
fn give_buf(pool: &mut Vec<Box<Vec<u8>>>, b: Buf) {
    if let Some(mut v) = b.0 {
        if pool.len() < POOL_MAX && v.capacity() <= 4 * OUT_CAP {
            v.clear();
            pool.push(v);
        }
    }
}

impl Server {
    /// Handles every completion of ring `ri`. Returns how many there were.
    fn drain(&mut self, ri: usize) -> usize {
        let Server { rings, app, pool, touched, listen, need_ring, .. } = self;
        let r = &mut rings[ri];
        let Ring { uring, bufs, conns, accepting, accepts, full, open, margin, rearm } = r;
        let slots = conns.len() as u32;
        let (sub, mut sq, mut cq) = uring.split();
        cq.sync();
        let mut n = 0;
        for cqe in &mut cq {
            n += 1;
            let u = cqe.user_data();
            let (op, gen, slot) = (u >> 56, (u >> 32) as u16, u as u32);
            let res = cqe.result();
            let flags = cqe.flags();
            match op {
                OP_ACCEPT => {
                    accepts[slot as usize] -= 1;
                    if res >= 0 {
                        let c = &mut conns[res as usize];
                        let g = c.gen.wrapping_add(1);
                        *c = Conn { gen: g, open: true, ..Conn::default() };
                        arm_recv(&mut sq, &sub, ri, res as u32, g);
                        *open += 1;
                        if *open + *margin >= slots {
                            *full = true;
                        }
                    } else if res == -libc::ENFILE {
                        *full = true;
                    }
                }
                OP_RECV => {
                    let bid = cqueue::buffer_select(flags);
                    let c = &mut conns[slot as usize];
                    if c.gen != gen || !c.open || c.closing {
                        if let Some(b) = bid {
                            bufs.add(b);
                        }
                        continue;
                    }
                    if res > 0 {
                        let Some(b) = bid else {
                            rearm.push(slot);
                            continue;
                        };
                        let data = bufs.get(b, res as usize);
                        serve(app, pool, touched, ri, slot, c, data);
                        bufs.add(b);
                        if !cqueue::more(flags) {
                            rearm.push(slot);
                        }
                    } else if res == -libc::ENOBUFS {
                        rearm.push(slot);
                    } else {
                        // EOF or error: finish what was asked, then close.
                        if let Some(b) = bid {
                            bufs.add(b);
                        }
                        c.close_after = true;
                        mark(touched, ri, slot, c);
                    }
                }
                OP_SEND => {
                    let c = &mut conns[slot as usize];
                    if c.gen != gen || !c.sending {
                        continue;
                    }
                    if res < 0 {
                        c.sending = false;
                        give_buf(pool, std::mem::take(&mut c.wire));
                        give_buf(pool, std::mem::take(&mut c.out));
                        c.close_after = true;
                        mark(touched, ri, slot, c);
                    } else {
                        c.sent += res as u32;
                        if (c.sent as usize) < c.wire.len() {
                            arm_send(&mut sq, &sub, ri, slot, c);
                        } else {
                            c.sending = false;
                            give_buf(pool, std::mem::take(&mut c.wire));
                            if !c.out.is_empty() || c.close_after {
                                // Sent after this batch commits, with its other responses.
                                mark(touched, ri, slot, c);
                            } else {
                                give_buf(pool, std::mem::take(&mut c.out));
                            }
                        }
                    }
                }
                OP_CLOSE => {
                    let c = &mut conns[slot as usize];
                    if c.gen == gen && c.open {
                        let g = c.gen;
                        give_buf(pool, std::mem::take(&mut c.out));
                        give_buf(pool, std::mem::take(&mut c.wire));
                        *c = Conn { gen: g, ..Conn::default() };
                        *open -= 1;
                    }
                    if *full && *open + 2 * *margin < slots {
                        *full = false;
                        *accepting = true;
                    }
                }
                _ => {}
            }
        }
        bufs.publish();
        if *accepting {
            if *full {
                // Move on; the accepts still in flight here fit in the margin.
                *accepting = false;
                *need_ring = true;
            } else {
                fill_accepts(&mut sq, &sub, ri, listen, accepts);
            }
        }
        n
    }

    /// Commits the batch, then hands every response it produced to the kernel.
    fn end_batch(&mut self) {
        let committed = self.app.db.commit();
        self.app.wrote = false;
        let Server { rings, touched, pool, .. } = self;
        for &(ri, slot) in touched.iter() {
            let r = &mut rings[ri as usize];
            let c = &mut r.conns[slot as usize];
            if !committed && c.answered {
                // No response from a rolled-back batch may claim success.
                let mark = c.batch_mark as usize;
                let out = c.out.get(pool);
                out.truncate(mark);
                app::error(out, 500, "internal server error");
                c.close_after = true;
            }
            c.in_batch = false;
            c.answered = false;
            if !c.open || c.closing || c.sending {
                continue;
            }
            let (sub, mut sq, _) = r.uring.split();
            if !c.out.is_empty() {
                let next = std::mem::take(&mut c.out);
                c.wire = next;
                c.sent = 0;
                c.sending = true;
                arm_send(&mut sq, &sub, ri as usize, slot, c);
            } else if c.close_after {
                arm_close(&mut sq, &sub, ri as usize, slot, c);
            }
        }
        touched.clear();
        for (ri, r) in rings.iter_mut().enumerate() {
            if r.rearm.is_empty() {
                continue;
            }
            let Ring { uring, conns, rearm, .. } = r;
            let (sub, mut sq, _) = uring.split();
            for &slot in rearm.iter() {
                let c = &conns[slot as usize];
                if c.open && !c.closing {
                    arm_recv(&mut sq, &sub, ri, slot, c.gen);
                }
            }
            rearm.clear();
        }
        if self.need_ring && std::time::Instant::now() >= self.spill_after {
            // Retried (once a second) until some ring accepts again, so accepting never stops.
            self.need_ring = !self.spill();
            if self.need_ring {
                self.spill_after = std::time::Instant::now() + std::time::Duration::from_secs(1);
            }
        }
    }

    /// A ring's descriptor table is full: accept on the next ring with room, adding one if needed.
    /// Returns whether some ring is accepting afterwards.
    fn spill(&mut self) -> bool {
        if self.rings.iter().any(|r| r.accepting) {
            return true;
        }
        if let Some(ri) = self.rings.iter().position(|r| !r.full) {
            arm_accept(&mut self.rings[ri], ri, &self.listen);
            return true;
        }
        if self.rings.len() >= MAX_RINGS {
            eprintln!("connection limit reached ({} rings)", self.rings.len());
            return false;
        }
        match Ring::new(self.slots) {
            Ok(mut r) => {
                let ri = self.rings.len();
                arm_accept(&mut r, ri, &self.listen);
                self.rings.push(r);
                eprintln!("ring {ri} added ({} slots each)", self.slots);
                true
            }
            Err(e) => {
                eprintln!("cannot add ring: {e}");
                false
            }
        }
    }

    fn run(&mut self) -> ! {
        loop {
            // Submit what the last batch queued and wait for completions. Deferred task work
            // (completions) only runs inside an enter with GETEVENTS. The other rings submit
            // and collect without waiting; ring 0 goes last and waits for WAIT_MIN
            // completions or WAIT_NS, so with several rings a pass still spans up to WAIT_NS
            // instead of spinning, and no ring's sends sit behind the wait.
            for (ri, r) in self.rings.iter_mut().enumerate().rev() {
                let sub = r.uring.submitter();
                let to_submit = unsafe { r.uring.submission_shared().len() } as u32;
                let rc = if ri == 0 {
                    let ts = types::Timespec::new().nsec(WAIT_NS);
                    let args = types::SubmitArgs::new().timespec(&ts);
                    sub.submit_with_args(WAIT_MIN, &args)
                } else {
                    unsafe { sub.enter::<libc::sigset_t>(to_submit, 0, GETEVENTS, None) }
                };
                if let Err(e) = rc {
                    match e.raw_os_error() {
                        Some(libc::EINTR | libc::ETIME | libc::EBUSY | libc::EAGAIN) => {}
                        _ => eprintln!("io_uring_enter: {e}"),
                    }
                }
            }
            let mut n = 0;
            for ri in 0..self.rings.len() {
                n += self.drain(ri);
            }
            let _ = n;
            #[cfg(feature = "stats")]
            stats::batch(n, self.touched.len(), self.app.wrote);
            self.end_batch();
        }
    }
}

/// `--features stats`: batch sizes on stderr every 5 s (not in the submitted build).
#[cfg(feature = "stats")]
mod stats {
    use std::cell::Cell;
    use std::time::Instant;
    thread_local! {
        static S: Cell<(u64, u64, u64, u64, Option<Instant>)> = const { Cell::new((0, 0, 0, 0, None)) };
    }
    pub fn batch(cqes: usize, responses: usize, wrote: bool) {
        S.with(|s| {
            let (mut b, mut c, mut r, mut w, t) = s.get();
            let t = t.unwrap_or_else(Instant::now);
            b += 1;
            c += cqes as u64;
            r += responses as u64;
            w += wrote as u64;
            if t.elapsed().as_secs() >= 5 {
                let secs = t.elapsed().as_secs_f64();
                eprintln!("stats: {:.0} loops/s, {:.1} cqes/loop, {:.1} conns answered/loop, {:.0} write commits/s", b as f64 / secs, c as f64 / b as f64, r as f64 / b as f64, w as f64 / secs);
                s.set((0, 0, 0, 0, Some(Instant::now())));
            } else {
                s.set((b, c, r, w, Some(t)));
            }
        });
    }
}

fn mark(touched: &mut Vec<(u8, u32)>, ri: usize, slot: u32, c: &mut Conn) {
    if !c.in_batch {
        c.in_batch = true;
        c.batch_mark = c.out.len() as u32;
        touched.push((ri as u8, slot));
    }
}

/// Parses and answers every complete request in `data` (plus any earlier partial bytes).
fn serve(app: &mut App, pool: &mut Vec<Box<Vec<u8>>>, touched: &mut Vec<(u8, u32)>, ri: usize, slot: u32, c: &mut Conn, data: &[u8]) {
    if c.close_after {
        return;
    }
    let mut partial: Vec<u8> = c.partial.0.take().map_or_else(Vec::new, |b| *b);
    let buf: &[u8] = if partial.is_empty() {
        data
    } else {
        partial.extend_from_slice(data);
        &partial
    };
    let mut off = 0;
    loop {
        match http::parse(&buf[off..]) {
            http::Parsed::Done(req, used) => {
                mark(touched, ri, slot, c);
                c.answered = true;
                app.handle(&req, c.out.get(pool));
                off += used;
                if off == buf.len() {
                    break;
                }
            }
            http::Parsed::Partial => break,
            http::Parsed::Bad => {
                mark(touched, ri, slot, c);
                c.answered = true;
                app::error(c.out.get(pool), 400, "bad request");
                c.close_after = true;
                off = buf.len();
                break;
            }
        }
    }
    let rest = &buf[off..];
    if rest.is_empty() {
        // Nothing left over: c.partial stays empty and `partial` is freed.
    } else if std::ptr::eq(buf.as_ptr(), data.as_ptr()) {
        c.partial = Buf(Some(Box::new(rest.to_vec())));
    } else {
        partial.drain(..off);
        c.partial = Buf(Some(Box::new(partial)));
    }
}

fn listen(host: &str, port: u16) -> io::Result<Vec<RawFd>> {
    let ip: std::net::Ipv4Addr = host.parse().map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "HOST must be an IPv4 address"))?;
    (0..LISTENERS).map(|_| listener(ip, port)).collect()
}

fn listener(ip: std::net::Ipv4Addr, port: u16) -> io::Result<RawFd> {
    unsafe {
        let fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let one: libc::c_int = 1;
        let sz = std::mem::size_of::<libc::c_int>() as libc::socklen_t;
        libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, &one as *const _ as *const _, sz);
        if libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_REUSEPORT, &one as *const _ as *const _, sz) < 0 {
            return Err(io::Error::last_os_error());
        }
        // Accepted sockets inherit TCP_NODELAY.
        libc::setsockopt(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, &one as *const _ as *const _, sz);
        let addr = libc::sockaddr_in {
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr { s_addr: u32::from(ip).to_be() },
            sin_zero: [0; 8],
        };
        if libc::bind(fd, &addr as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t) < 0 {
            return Err(io::Error::last_os_error());
        }
        if libc::listen(fd, 65535) < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(fd)
    }
}

/// Raises the soft descriptor limit to the hard one. A ring's file table may be as large as it.
fn descriptor_slots() -> u32 {
    unsafe {
        let mut rl: libc::rlimit = std::mem::zeroed();
        libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl);
        rl.rlim_cur = rl.rlim_max;
        libc::setrlimit(libc::RLIMIT_NOFILE, &rl);
        libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl);
        rl.rlim_cur.min(1 << 20) as u32
    }
}

fn main() {
    let env = |k: &str, d: &str| std::env::var(k).unwrap_or_else(|_| d.to_string());
    let path = env("SQLITE_PATH", "feed.db");
    let secret = env("JWT_SECRET", "");
    let host = env("HOST", "127.0.0.1");
    let port: u16 = env("PORT", "3000").parse().expect("PORT");
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };
    let db = db::Db::open(&path).unwrap_or_else(|e| {
        eprintln!("database: {e}");
        std::process::exit(1)
    });
    let app = App::new(db, secret.as_bytes());
    let slots = descriptor_slots();
    let fd = listen(&host, port).unwrap_or_else(|e| {
        eprintln!("listen {host}:{port}: {e}");
        std::process::exit(1)
    });
    let mut ring = Ring::new(slots).unwrap_or_else(|e| {
        eprintln!("io_uring: {e}");
        std::process::exit(1)
    });
    arm_accept(&mut ring, 0, &fd);
    eprintln!("listening on {host}:{port}; io_uring, {slots} direct descriptors per ring");
    let mut s = Server { rings: vec![ring], app, listen: fd, slots, pool: Vec::new(), touched: Vec::new(), need_ring: false, spill_after: std::time::Instant::now() };
    s.run()
}
