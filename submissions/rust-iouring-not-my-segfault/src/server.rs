use crate::auth::{Auth, AuthError, Claims};
use crate::conn::{self, ConnId, Conns, End, Op, Pause, Reply, Unacked, flag};
use crate::http::{self, Body, ErrorBody, Parse, Request, Route, Span, Status, Version};
use crate::json::{self, BodyError};
use crate::ring::{
    self, Armed, CloseIo, CloseLinger, Completion, RecvMode, Rings, Submit, WaitEnd,
};
use crate::stats::{Progress, Stats, stderr_line};
use crate::store::{Read, Store, Write as StoreWrite};
use crate::wal::{Wal, WalStep};
use std::collections::VecDeque;
use std::io;
use std::net::TcpListener;
use std::os::fd::AsFd;
use std::ptr::NonNull;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const WRITE_QUEUE: usize = if cfg!(twelve_small_limits) { 8 } else { 4_096 };
const Q: usize = if cfg!(twelve_small_limits) { 8 } else { 64 };
const BODY_MAX: usize = 131_072;
const WHOLE_MAX: usize = BODY_MAX + http::HEAD_ROOM;
// The 100 ms sample retries an accept paused by an operational error; it reads no file.
const SAMPLE_PERIOD: Duration = Duration::from_millis(100);
const STATS_PERIOD: Duration = Duration::from_secs(10);
// The counters the loop gathers from rings, store and WAL, and its peak samples, are folded into the
// stats every 100 ms and before each written stats line, not every turn.
const COUNTS_PERIOD: Duration = Duration::from_millis(100);
// The write window adapts once per 250 ms epoch; the process CPU clock is read only then.
const ADAPT_EPOCH: Duration = Duration::from_millis(250);
const SWEEP_PERIOD: Duration = Duration::from_secs(if cfg!(twelve_small_limits) { 1 } else { 10 });
// The safe build follows every rule an unsafe path of this file skips; every use is an
// ordinary `if SAFE_BUILD`, so both builds compile and type-check both branches.
const SAFE_BUILD: bool = cfg!(twelve_safe);
const NONE: u32 = u32::MAX;
const VALID: u32 = 1;
const HEAD: u32 = 1 << 1;
const EARLY: u32 = 1 << 2;
const RENDERED: u32 = 1 << 3;
const FINISHED: u32 = 1 << 4;
const DEPENDENT: u32 = 1 << 5;
const MUTATED: u32 = 1 << 6;
const EXECUTED: u32 = 1 << 7;

// Copy: a turn reads a queued work out, updates it and writes it back, with no empty placeholder.
#[repr(C)]
#[derive(Clone, Copy)]
struct Work {
    id: ConnId,
    route: Route,
    receipt: Instant,
    ordinal: u64,
    user_id: i64,
    name: Span,
    body: Span,
    consumed: u32,
    held: u32,
    base: u32,
    body_start: u32,
    head_start: u32,
    need: u32,
    flags: u32,
    status: Status,
    error: Option<ErrorBody>,
    close: bool,
    version: Version,
    // A write's reply bytes (Store::create_need or like_need), computed once when the write is
    // admitted; None when the reply could not fit BODY_MAX, and for every other work.
    reply_need: Option<u32>,
    padding: [u8; 16],
}
const _: () = assert!(size_of::<Work>() == 128 && align_of::<Work>() == 8);

impl Work {
    fn empty(now: Instant) -> Self {
        Self {
            id: ConnId {
                ring: 0,
                slot: 0,
                generation: 0,
            },
            route: Route::NotFound,
            receipt: now,
            ordinal: 0,
            user_id: 0,
            name: Span { start: 0, len: 0 },
            body: Span { start: 0, len: 0 },
            consumed: 0,
            held: 0,
            base: 0,
            body_start: 0,
            head_start: 0,
            need: 0,
            flags: 0,
            status: Status::Internal,
            error: None,
            close: false,
            version: Version::Http11,
            reply_need: None,
            padding: [0; 16],
        }
    }
    fn write(&self) -> bool {
        matches!(self.route, Route::Create | Route::Like(_)) && self.error.is_none()
    }
}

const PUSH_ONCE: u32 = 1;
const PUSH_SEND: u32 = 1 << 1;
const PUSH_RECV: u32 = 1 << 2;
const PUSH_SINGLE: u32 = 1 << 3;
const PUSH_POLL: u32 = 1 << 4;
const CANCEL_RECV: u32 = 1 << 5;
const CANCEL_POLL: u32 = 1 << 6;
const PUSH_CLOSE: u32 = 1 << 7;
const PUSH_ORPHAN: u32 = 1 << 8;
const CLOSE_INTENT: u32 = 1 << 9;
const POLL_READY: u32 = 1 << 10;
const BLOCKED: u32 = 1 << 11;
const OUTPUT_PAUSE: u32 = 1 << 12;
const OUTPUT_COUNTED: u32 = 1 << 13;
const QUEUE_COUNTED: u32 = 1 << 14;
const INPUT_COUNTED: u32 = 1 << 15;
const PIPE_COUNTED: u32 = 1 << 16;
const HALF_COUNTED: u32 = 1 << 17;
const LINKED: u32 = 1 << 18;
const REARM_COUNT: u32 = 1 << 19;
// The safe build's drain: the write side's shutdown is owed, pushed after the cancels, before close.
const PUSH_SHUTDOWN: u32 = 1 << 20;
// The fast build's SIOCOUTQ ask of a cell close: OUTQ_WAIT while the question is in flight,
// OUTQ_DONE once asked, OUTQ_ZERO when the kernel reported no unacknowledged send bytes.
const OUTQ_WAIT: u32 = 1 << 21;
const OUTQ_DONE: u32 = 1 << 22;
const OUTQ_ZERO: u32 = 1 << 23;
// A partial request whose input reservation could not grow: it keeps its reservation, its bytes
// and their order, and waits for input credit; pending_turn retries the growth.
const GROW_WAIT: u32 = 1 << 24;
// Reads before closes. In a turn that reaped any
// receive completion, at most CLOSE_TRICKLE lean close pushes (SIOCOUTQ or Close) start, so a
// flood of ends of input is read before its closes allocate their reset skbs; a turn that reaped
// none pushes up to CLOSE_BULK_RING a ring. CLOSE_TRICKLE: outside hang-ups the recorded runs held
// at most 3 closes in any 10 s, and a loaded turn lasts at least
// the 2 ms wake wait, so 64 a turn (32,000 a second) never makes an ordinary close wait.
// CLOSE_BULK_RING: the SQ's 4,096 entries; a push makes at most two completions, so at most 8,192
// of a ring's 16,384 CQ entries per turn, and these closes never push the CQ into the kernel's
// overflow list, whose per-entry GFP_ATOMIC allocation loses the completion when it fails.
pub(crate) const CLOSE_TRICKLE: usize = 64;
pub(crate) const CLOSE_BULK_RING: usize = 4_096;
const PUSH_MASK: u32 = PUSH_ONCE
    | PUSH_SEND
    | PUSH_RECV
    | PUSH_SINGLE
    | PUSH_POLL
    | CANCEL_RECV
    | CANCEL_POLL
    | PUSH_SHUTDOWN
    | PUSH_CLOSE
    | PUSH_ORPHAN;

#[derive(Clone, Copy, Default)]
enum CloseReason {
    #[default]
    Intentional,
    Eof,
    Reset,
    Error,
    Stall,
}

#[repr(C)]
struct Pending {
    work: Work,
    name: Vec<u8>,
    once: Option<NonNull<u8>>,
    send: Option<NonNull<u8>>,
    id: ConnId,
    once_len: u32,
    send_len: u32,
    prev: u32,
    next: u32,
    bits: u32,
    acks: u32,
    old_generation: u32,
    old_acks: u32,
    attempted: u64,
    reason: CloseReason,
    padding: [u8; 39],
}
const _: () = assert!(size_of::<Pending>() == 256 && align_of::<Pending>() == 8);
impl Pending {
    fn empty(now: Instant) -> Self {
        Self {
            work: Work::empty(now),
            name: Vec::new(),
            once: None,
            send: None,
            id: ConnId {
                ring: 0,
                slot: 0,
                generation: 0,
            },
            once_len: 0,
            send_len: 0,
            prev: NONE,
            next: NONE,
            bits: 0,
            acks: 0,
            old_generation: 0,
            old_acks: 0,
            attempted: 0,
            reason: CloseReason::Intentional,
            padding: [0; 39],
        }
    }
}
struct Metadata {
    // One flat array of every cell made so far, at most one per slot of the 8 rings: its capacity is
    // reserved at startup and touched only as cells are pushed, so a cell never moves and no cell
    // memory is freed and remade under load; a released cell is reset and reused from free, the most
    // recently released first.
    cells: Vec<Pending>,
    free: Vec<u32>,
    first: u32,
    last: u32,
    queued: usize,
    kept_name: Vec<u8>,
    names: usize,
    // INPUT_BUDGET in bytes as usize, converted once here.
    names_max: usize,
    output_paused: u64,
    now: Instant,
}
impl Metadata {
    fn new(slots: u32, now: Instant) -> Self {
        let count = slots as usize * ring::RINGS;
        Self {
            cells: Vec::with_capacity(count),
            free: Vec::with_capacity(count),
            first: NONE,
            last: NONE,
            queued: 0,
            kept_name: Vec::new(),
            names: 0,
            names_max: usize::try_from(conn::INPUT_BUDGET)
                .unwrap_or_else(|_| invariant("capacity")),
            output_paused: 0,
            now,
        }
    }
    fn index(&self, conns: &Conns, id: ConnId) -> Option<u32> {
        conns.cell(id)
    }
    fn cell(&self, index: u32) -> &Pending {
        self.cells
            .get(index as usize)
            .expect("a cell index names a cell made by ensure")
    }
    fn cell_mut(&mut self, index: u32) -> &mut Pending {
        self.cells
            .get_mut(index as usize)
            .expect("a cell index names a cell made by ensure")
    }
    fn get(&self, conns: &Conns, id: ConnId) -> Option<&Pending> {
        self.index(conns, id)
            .map(|index| self.cell(index))
            .filter(|p| p.id == id)
    }
    fn ensure(&mut self, conns: &Conns, id: ConnId) -> u32 {
        if let Some(index) = self.index(conns, id) {
            if self.cell(index).id != id {
                let old = self.cell(index);
                if old.bits & PUSH_MASK != 0 || old.work.flags & VALID != 0 || old.old_acks != 0 {
                    invariant("operation");
                }
                let (old_generation, old_acks) = (old.id.generation, old.acks);
                let cell = self.cell_mut(index);
                cell.id = id;
                cell.old_generation = old_generation;
                cell.old_acks = old_acks;
                cell.acks = 0;
                cell.bits &= LINKED;
            }
            return index;
        }
        let index = match self.free.pop() {
            Some(index) => index,
            None => {
                // Each cell is free or named by one physical slot. This slot names none, so
                // the push stays inside the capacity reserved at startup.
                let index = u32n(self.cells.len(), "capacity");
                self.cells.push(Pending::empty(self.now));
                index
            }
        };
        conns.set_cell(id, Some(index));
        self.cell_mut(index).id = id;
        index
    }
    fn enqueue(&mut self, index: u32) {
        if self.cell(index).bits & LINKED != 0 {
            return;
        }
        let last = self.last;
        let cell = self.cell_mut(index);
        cell.bits |= LINKED;
        cell.prev = last;
        cell.next = NONE;
        if last == NONE {
            self.first = index;
        } else {
            self.cell_mut(last).next = index;
        }
        self.last = index;
        // UNSAFE PATH (speed): unchecked arithmetic; queued counts the cells linked in the FIFO, at most 8 * slots.
        self.queued += 1;
    }
    fn unlink(&mut self, index: u32) {
        if self.cell(index).bits & LINKED == 0 {
            return;
        }
        let (prev, next) = (self.cell(index).prev, self.cell(index).next);
        if prev == NONE {
            self.first = next;
        } else {
            self.cell_mut(prev).next = next;
        }
        if next == NONE {
            self.last = prev;
        } else {
            self.cell_mut(next).prev = prev;
        }
        let cell = self.cell_mut(index);
        cell.bits &= !LINKED;
        cell.prev = NONE;
        cell.next = NONE;
        // UNSAFE PATH (speed): unchecked arithmetic; queued counts the cells linked in the FIFO, at most 8 * slots.
        self.queued -= 1;
    }
    fn clear_request(&mut self, index: u32) {
        let bits = self.cell(index).bits;
        if bits & OUTPUT_PAUSE != 0 {
            // UNSAFE PATH (speed): unchecked arithmetic; output_paused counts the cells holding OUTPUT_PAUSE, at most 8 * slots.
            self.output_paused -= 1;
        }
        self.release_name(index);
        let cell = self.cell_mut(index);
        // A cell's work is read only while its flags hold VALID or HEAD, and every new request's
        // work is built whole (request_work), so clearing the flags empties it.
        cell.work.flags = 0;
        cell.bits &= !(BLOCKED
            | OUTPUT_PAUSE
            | OUTPUT_COUNTED
            | QUEUE_COUNTED
            | INPUT_COUNTED
            | PIPE_COUNTED
            | HALF_COUNTED
            | GROW_WAIT);
    }
    fn release_name(&mut self, index: u32) {
        let mut name = std::mem::take(&mut self.cell_mut(index).name);
        // UNSAFE PATH (speed): unchecked arithmetic; names is the sum of live name capacities, at most INPUT_BUDGET.
        self.names -= name.capacity();
        name.clear();
        if name.capacity() <= conn::KEEP_MAX as usize && name.capacity() > self.kept_name.capacity()
        {
            self.kept_name = name;
        }
    }
    fn save_name(&mut self, index: u32, bytes: &[u8]) {
        self.release_name(index);
        let available = self.names_max.strict_sub(self.names);
        // The encoded Authorization value covers up to 4/3 of its decoded name length.
        let name_bound = bytes.len().strict_mul(4) / 3;
        let mut name = if self.kept_name.capacity() >= bytes.len()
            && self.kept_name.capacity() <= available
            && self.kept_name.capacity() <= name_bound
        {
            std::mem::take(&mut self.kept_name)
        } else {
            Vec::with_capacity(bytes.len())
        };
        name.extend_from_slice(bytes);
        self.names = self
            .names
            .checked_add(name.capacity())
            .unwrap_or_else(|| invariant("capacity"));
        // Actual capacities, including reusable slack, stay within INPUT_BUDGET. A fresh name
        // fits because decoded live names occupy at most 3/4 of their charged request heads.
        // UNSAFE PATH (speed): unchecked arithmetic; names is the sum of live name capacities, at most INPUT_BUDGET.
        if self.names + self.kept_name.capacity() > self.names_max {
            invariant("capacity");
        }
        self.cell_mut(index).name = name;
    }
    fn release(&mut self, conns: &Conns, index: u32) {
        self.unlink(index);
        self.clear_request(index);
        let id = self.cell(index).id;
        conns.set_cell(id, None);
        let now = self.now;
        *self.cell_mut(index) = Pending::empty(now);
        self.free.push(index);
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Ingress {
    generation: u32,
    slot: u16,
    bid: u16,
    len: u16,
    offset: u16,
    next: u16,
    ring: u8,
    additional: u8,
}
const _: () = assert!(size_of::<Ingress>() == 16);
struct Ingresses {
    count: u16,
    heads: Box<[[u16; 2]]>,
    nodes: Box<[Ingress]>,
    held: [u32; 8],
    slots: u32,
}
impl Ingresses {
    fn new(slots: u32, count: u16) -> Self {
        Self {
            count,
            heads: vec![[0; 2]; slots as usize * 8].into_boxed_slice(),
            nodes: vec![Ingress::default(); usize::from(count) * 8].into_boxed_slice(),
            held: [0; 8],
            slots,
        }
    }
    fn key(&self, id: ConnId) -> usize {
        usize::from(id.ring) * self.slots as usize + usize::from(id.slot)
    }
    fn node(&self, ring: u8, bid: u16) -> usize {
        usize::from(ring) * usize::from(self.count) + usize::from(bid)
    }
    fn head(&self, key: usize) -> [u16; 2] {
        *self
            .heads
            .get(key)
            .expect("key < 8 * slots: the ConnId's ring and slot are in range")
    }
    fn head_mut(&mut self, key: usize) -> &mut [u16; 2] {
        self.heads
            .get_mut(key)
            .expect("key < 8 * slots: the ConnId's ring and slot are in range")
    }
    fn entry(&self, index: usize) -> Ingress {
        *self
            .nodes
            .get(index)
            .expect("node < 8 * bufs_per_ring: ring < 8 and bid < bufs_per_ring")
    }
    fn entry_mut(&mut self, index: usize) -> &mut Ingress {
        self.nodes
            .get_mut(index)
            .expect("node < 8 * bufs_per_ring: ring < 8 and bid < bufs_per_ring")
    }
    fn held_mut(&mut self, ring: u8) -> &mut u32 {
        self.held.get_mut(usize::from(ring)).expect("ring < 8")
    }
    fn has(&self, id: ConnId) -> bool {
        self.head(self.key(id))[0] != 0
    }
    fn insert(
        &mut self,
        id: ConnId,
        bid: u16,
        len: u16,
        offset: u16,
        first: bool,
        stats: &mut Stats,
    ) {
        let key = self.key(id);
        let index = self.node(id.ring, bid);
        if self.entry(index).len != 0 || offset >= len {
            invariant("ingress");
        }
        let next = if first { self.head(key)[0] } else { 0 };
        *self.entry_mut(index) = Ingress {
            generation: id.generation,
            slot: id.slot,
            bid,
            len,
            offset,
            next,
            ring: id.ring,
            additional: u8::from(!first),
        };
        if first {
            let head = self.head_mut(key);
            head[0] = bid + 1;
            if head[1] == 0 {
                head[1] = bid + 1;
            }
        } else {
            let tail = self.head(key)[1];
            if tail == 0 {
                self.head_mut(key)[0] = bid + 1;
            } else {
                let tail_index = self.node(id.ring, tail - 1);
                self.entry_mut(tail_index).next = bid + 1;
            }
            self.head_mut(key)[1] = bid + 1;
            let held = self.held_mut(id.ring);
            // UNSAFE PATH (speed): unchecked arithmetic; held counts one ring's held bids, at most bufs_per_ring.
            *held += 1;
            let held = *held;
            add(&mut stats.ingress_held, 1);
            stats.ingress_held_max = stats.ingress_held_max.max(u64::from(held));
        }
    }
    fn front(&self, id: ConnId) -> Option<Ingress> {
        let head = self.head(self.key(id))[0];
        (head != 0).then(|| self.entry(self.node(id.ring, head - 1)))
    }
    fn pop(&mut self, id: ConnId) -> Option<Ingress> {
        let node = self.front(id)?;
        let key = self.key(id);
        let head = self.head_mut(key);
        head[0] = node.next;
        if node.next == 0 {
            head[1] = 0;
        }
        let index = self.node(id.ring, node.bid);
        *self.entry_mut(index) = Ingress::default();
        if node.additional != 0 {
            // UNSAFE PATH (speed): unchecked arithmetic; held counts one ring's held bids, at most bufs_per_ring.
            *self.held_mut(id.ring) -= 1;
        }
        Some(node)
    }
}

struct Window {
    us: u64,
    growing: bool,
    epoch: Instant,
    cpu: u128,
    finals: u128,
    latency: u128,
    writes: u128,
    previous: Option<(u128, u128, u128, u128)>,
}
impl Window {
    fn new(now: Instant) -> Self {
        Self {
            us: 4_000,
            growing: true,
            epoch: now,
            cpu: 0,
            finals: 0,
            latency: 0,
            writes: 0,
            previous: None,
        }
    }
    // Whether a 250 ms epoch has passed since the last adapt: the caller reads the process CPU clock,
    // a system call, only then.
    fn due(&self, now: Instant) -> bool {
        now.duration_since(self.epoch) >= ADAPT_EPOCH
    }
    fn adapt(&mut self, now: Instant, cpu: u128, finals: u128) {
        if !self.due(now) {
            return;
        }
        let requests = finals
            .checked_sub(self.finals)
            .unwrap_or_else(|| invariant("clock-range"));
        let elapsed_cpu = cpu
            .checked_sub(self.cpu)
            .unwrap_or_else(|| invariant("clock-range"));
        if requests != 0 {
            if let Some((old_cpu, old_requests, old_latency, old_writes)) = self.previous {
                let cpu_rose = product(elapsed_cpu, old_requests) > product(old_cpu, requests);
                let latency_rose = self.growing
                    && old_writes != 0
                    && self.writes != 0
                    && product(self.latency, old_writes)
                        > product(sum(old_latency, product(500_000, old_writes)), self.writes);
                if cpu_rose || latency_rose {
                    self.growing = !self.growing;
                }
            }
            self.us = if self.growing {
                (self.us + 1_000).min(10_000)
            } else {
                self.us.saturating_sub(1_000).max(4_000)
            };
            self.previous = Some((elapsed_cpu, requests, self.latency, self.writes));
        }
        self.epoch = now;
        self.cpu = cpu;
        self.finals = finals;
        self.latency = 0;
        self.writes = 0;
    }
}
#[derive(Clone, Copy)]
enum AcceptState {
    Unarmed,
    Armed {
        ring: u8,
        arm: u32,
    },
    Cancelling {
        ring: u8,
        arm: u32,
        successor: Option<u8>,
    },
    CapacityPaused,
    ErrorPausedUntilSample,
}

/// Whether accepted sockets inherit the listener's zero linger: Inherited when main's startup check
/// found that a socket accepted from a listener holding SO_LINGER {1, 0} inherits it and main set
/// that option on the real listener; PerClose otherwise, and always in the safe build.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ListenerLinger {
    Inherited,
    PerClose,
}

pub(crate) struct Server {
    // Drop order: rings drops first, after a teardown's rings.drain() has ended every in-flight operation, so conns frees no buffer the kernel can still use; Rings' Drop itself never waits. wal drops before store, whose database the checkpointer handle uses.
    rings: Rings,
    listener: TcpListener,
    wal: Wal,
    store: Store,
    conns: Conns,
    auth: Auth,
    stats: Stats,
    progress: Arc<Progress>,
    listener_linger: ListenerLinger,
    // The close backlog: closes decided whose Close completion has not arrived, at most 8 * slots.
    close_wait: u32,
    // The turn's lift, decided once right after wait() returns: close_wait above the turn cap.
    lifted: bool,
    // Receive completions reaped this turn (reads before closes).
    recv_reaped: usize,
    batch_commit: bool,
    poll_recv: bool,
    cap: usize,
    // The effective turn cap as main computed it; cap is the same value as usize.
    turn_cap: u32,
    batch_target: u32,
    ring_spread: crate::settings::RingSpread,
    rearm_budget: [u32; 8],
    timeout: Duration,
    start: Instant,
    // The turn's one monotonic clock reading, taken right after wait() returns; receipts, timers, the
    // gate's hold samples, commit starts and the turn timer read it, and the next wait() computes
    // its timeout from it.
    now: Instant,
    // UTC offset from monotonic time since start, refreshed by the 100 ms sample.
    wall_offset_ns: i128,
    wall: Option<i64>,
    next_sample: Instant,
    next_sweep: Instant,
    next_stats: Instant,
    next_counts: Instant,
    window: Window,
    completions: Vec<(Completion, Option<End>)>,
    candidates: Vec<ConnId>,
    batch: Vec<Work>,
    writes: VecDeque<Work>,
    metadata: Metadata,
    ingress: Ingresses,
    claims: Claims,
    chunk: Vec<u8>,
    unescape: Vec<u8>,
    accept: AcceptState,
    accept_owner: u8,
    accept_cancel_queued: bool,
    accept_attempted: bool,
    // An fsync of the descriptor wal.sync_fd() lends is owed to the ring.
    sync_pending: bool,
    sync_in_flight: bool,
    ring_blocked: [bool; 8],
    turn: u64,
    ordinal: u64,
    finals: u128,
    cache: (u64, u64),
    busy: bool,
    advanced: bool,
    // A write joined the queue this turn: the window's idle rule waits for a turn without one.
    write_joined: bool,
    left: bool,
    refill: bool,
    #[cfg(twelve_fault_inject)]
    sleep_path: std::path::PathBuf,
    #[cfg(twelve_fault_inject)]
    slept: bool,
}

fn add(target: &mut u64, value: u64) {
    *target = target.saturating_add(value);
}
fn sum(a: u128, b: u128) -> u128 {
    a.checked_add(b).unwrap_or_else(|| invariant("clock-range"))
}
fn product(a: u128, b: u128) -> u128 {
    a.checked_mul(b).unwrap_or_else(|| invariant("clock-range"))
}
fn u32n(value: usize, kind: &str) -> u32 {
    u32::try_from(value).unwrap_or_else(|_| invariant(kind))
}
fn duration_us(value: Duration) -> u64 {
    u64::try_from(value.as_micros()).unwrap_or(u64::MAX)
}
fn runtime(detail: impl std::fmt::Display) -> ! {
    fatal("server runtime: ", detail);
}
fn invariant(kind: &str) -> ! {
    fatal("server invariant: ", kind);
}
fn fatal(prefix: &str, detail: impl std::fmt::Display) -> ! {
    // One write, whole or not at all (stderr_line); the process ends with status 1 either way.
    let _ = stderr_line(|out| {
        write!(out, "{prefix}")?;
        for scalar in detail.to_string().chars() {
            write!(
                out,
                "{}",
                if matches!(scalar, '\r' | '\n') {
                    ' '
                } else {
                    scalar
                }
            )?;
        }
        writeln!(out)
    });
    std::process::exit(1)
}
fn transport<T>(result: io::Result<T>) -> T {
    result.unwrap_or_else(|error| runtime(error))
}
fn cpu_time() -> u128 {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: time is an initialized, aligned writable timespec. This process's
    // CPU clock is read synchronously, with no retained pointer or shared state.
    if unsafe { libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut time) } != 0 {
        runtime("clock");
    }
    if time.tv_sec < 0 || !(0..1_000_000_000).contains(&time.tv_nsec) {
        runtime("clock");
    }
    let seconds = u128::try_from(time.tv_sec).unwrap_or_else(|_| invariant("clock-range"));
    let nanoseconds = u128::try_from(time.tv_nsec).unwrap_or_else(|_| invariant("clock-range"));
    sum(product(seconds, 1_000_000_000), nanoseconds)
}
fn unix_nanos() -> i128 {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(value) => i128::try_from(value.as_nanos()).unwrap_or_else(|_| runtime("clock")),
        Err(error) => {
            -i128::try_from(error.duration().as_nanos()).unwrap_or_else(|_| runtime("clock"))
        }
    }
}
fn unix_seconds() -> i64 {
    i64::try_from(unix_nanos().div_euclid(1_000_000_000)).unwrap_or_else(|_| runtime("clock"))
}
fn range(span: Span, len: usize) -> std::ops::Range<usize> {
    let start = span.start as usize;
    let end = start
        .checked_add(span.len as usize)
        .filter(|end| *end <= len)
        .unwrap_or_else(|| invariant("framing"));
    start..end
}
fn text(bytes: &[u8], span: Span) -> &str {
    let bytes = bytes
        .get(range(span, bytes.len()))
        .unwrap_or_else(|| invariant("framing"));
    std::str::from_utf8(bytes).unwrap_or_else(|_| invariant("framing"))
}
// The text of a write's name or body span, which admit already checked as UTF-8 (write_need's text)
// and nothing rewrites until the reply is released: validate's rewrites come first, and compacting
// or growing the input moves the bytes with their span offsets.
fn admitted_text(bytes: &[u8], span: Span) -> &str {
    if SAFE_BUILD {
        return text(bytes, span);
    }
    let bytes = bytes
        .get(range(span, bytes.len()))
        .unwrap_or_else(|| invariant("framing"));
    // UNSAFE PATH (speed): UTF-8 checked once; a write's name and body are checked when the write is admitted and read unchecked when it executes, since twelve-dollar-lab RESULT.md's "Speed profile at 310,000" measures core::str::from_utf8 at 0.021 us a request.
    // SAFETY: from_utf8_unchecked needs valid UTF-8: admit's write_need ran text over this same span
    // of this request's input, which exits on invalid UTF-8, and the bytes are unchanged since.
    unsafe { std::str::from_utf8_unchecked(bytes) }
}
// Starts loading the cache line at address, the connection record of a reaped completion, so that the
// turn's first read of it finds it in the cache.
fn prefetch(address: *const u8) {
    if SAFE_BUILD {
        return;
    }
    // UNSAFE PATH (speed): unsafe code for a cache hint; the fast build prefetches each reaped completion's connection record, a random 64-byte line of a 32 MiB table, while twelve-dollar-lab RESULT.md's "Speed profile at 310,000" measures Conns::get at 0.216 us a request.
    // SAFETY: a prefetch is a hint that never faults and changes no memory; address is computed by
    // Conns::record_address inside its live table and is not dereferenced.
    unsafe {
        std::arch::x86_64::_mm_prefetch::<{ std::arch::x86_64::_MM_HINT_T0 }>(address.cast())
    };
}
// The ring and slot of the completions that name a connection on the hot path, for prefetch.
fn completion_slot(event: Completion) -> Option<(u8, u16)> {
    match event {
        Completion::Recv { ring, slot, .. }
        | Completion::RecvOnce { ring, slot, .. }
        | Completion::Send { ring, slot, .. }
        | Completion::Poll { ring, slot, .. }
        | Completion::Close { ring, slot, .. } => Some((ring, slot)),
        _ => None,
    }
}
fn escaped_len(bytes: &[u8]) -> Option<usize> {
    bytes.iter().try_fold(2_usize, |size, byte| {
        size.checked_add(match byte {
            b'"' | b'\\' | b'\n' | b'\r' | b'\t' => 2,
            0..=31 => 6,
            _ => 1,
        })
    })
}
fn decimal(out: &mut Vec<u8>, mut value: u64) {
    let mut bytes = [0; 20];
    let mut at = bytes.len();
    loop {
        at -= 1;
        *bytes
            .get_mut(at)
            .expect("at < 20: a u64 has at most 20 decimal digits") =
            b'0' + u8::try_from(value % 10).expect("decimal digit");
        value /= 10;
        if value == 0 {
            break;
        }
    }
    out.extend_from_slice(
        bytes
            .get(at..)
            .expect("at < 20: a u64 has at most 20 decimal digits"),
    );
}

impl Server {
    #[expect(
        clippy::too_many_arguments,
        reason = "The composition boundary transfers these distinct owners."
    )]
    pub(crate) fn new(
        settings: &crate::Settings,
        listener: TcpListener,
        rings: Rings,
        conns: Conns,
        store: Store,
        mut wal: Wal,
        auth: Auth,
        stats: Stats,
        progress: Arc<Progress>,
        listener_linger: ListenerLinger,
    ) -> Self {
        let start = Instant::now();
        let slots = rings.slots();
        let buffers = rings.startup().bufs_per_ring;
        wal.seed_frames(store.wal_frames(), start);
        let turn_cap = settings.effective_turn_cap();
        let cap = turn_cap as usize;
        Self {
            rings,
            listener,
            wal,
            store,
            conns,
            auth,
            stats,
            progress,
            listener_linger,
            close_wait: 0,
            lifted: false,
            recv_reaped: 0,
            batch_commit: settings.batch_commit(),
            poll_recv: settings.poll_recv(),
            cap,
            turn_cap,
            batch_target: settings.batch_target(),
            ring_spread: settings.ring_spread(),
            rearm_budget: [0; 8],
            timeout: Duration::from_micros(u64::from(settings.wake_wait_us())),
            start,
            now: start,
            wall_offset_ns: unix_nanos(),
            wall: None,
            next_sample: start + SAMPLE_PERIOD,
            next_sweep: start + SWEEP_PERIOD,
            next_stats: start + STATS_PERIOD,
            next_counts: start + COUNTS_PERIOD,
            window: Window::new(start),
            completions: Vec::with_capacity(cap),
            candidates: Vec::with_capacity(cap),
            batch: Vec::with_capacity(cap),
            writes: VecDeque::with_capacity(WRITE_QUEUE),
            metadata: Metadata::new(slots, start),
            ingress: Ingresses::new(slots, buffers),
            claims: Claims::new(),
            chunk: Vec::with_capacity(conn::KEEP_MAX as usize),
            unescape: Vec::with_capacity(conn::KEEP_MAX as usize),
            accept: AcceptState::Unarmed,
            accept_owner: 0,
            accept_cancel_queued: false,
            accept_attempted: false,
            sync_pending: false,
            sync_in_flight: false,
            ring_blocked: [false; 8],
            turn: 0,
            ordinal: 0,
            finals: 0,
            busy: false,
            advanced: false,
            write_joined: false,
            left: false,
            refill: true,
            cache: (0, 0),
            #[cfg(twelve_fault_inject)]
            sleep_path: std::path::PathBuf::from(format!(
                "{}-fault-server-loop-sleep",
                settings.sqlite_path()
            )),
            #[cfg(twelve_fault_inject)]
            slept: false,
        }
    }

    pub(crate) fn run(mut self) -> ! {
        self.window.cpu = cpu_time();
        self.maintain_accept();
        self.submit();
        loop {
            self.turn = self.turn.wrapping_add(1);
            // Relaxed: this atomic publishes only its own value; no other data is read through it.
            self.progress.turn.store(self.turn, Ordering::Relaxed);
            self.ring_blocked = [false; 8];
            self.accept_attempted = false;
            self.advanced = false;
            self.write_joined = false;
            self.refill = self.batch.is_empty();
            self.candidates.clear();
            let ready = if self.refill {
                self.conns.ready().min(self.turn_cap)
            } else {
                0
            };
            self.conns.take_ready(ready, |id| self.candidates.push(id));
            self.step(1);
            self.wait();
            // The turn's one clock reading: the turn timer spans from the last turn's reading to
            // this one, one wait and one turn of work.
            let previous = self.now;
            self.now = Instant::now();
            let wall_ns = self
                .wall_offset_ns
                .checked_add(
                    i128::try_from(self.now.duration_since(self.start).as_nanos())
                        .unwrap_or_else(|_| invariant("clock-range")),
                )
                .unwrap_or_else(|| invariant("clock-range"));
            self.wall = Some(
                i64::try_from(wall_ns.div_euclid(1_000_000_000))
                    .unwrap_or_else(|_| invariant("clock-range")),
            );
            self.metadata.now = self.now;
            self.store.tick(self.now);
            self.conns.tick(self.seconds(self.now));
            let turn_us = duration_us(self.now.saturating_duration_since(previous));
            self.stats.turn_us_max = self.stats.turn_us_max.max(turn_us);
            self.lifted = self.close_wait > self.turn_cap;
            self.completions.clear();
            let reap_cap = if self.lifted { usize::MAX } else { self.cap };
            let (completions, conns) = (&mut self.completions, &self.conns);
            let reaped = self.rings.reap(reap_cap, |event| {
                if let Some((ring, slot)) = completion_slot(event)
                    && let Some(address) = conns.record_address(ring, slot)
                {
                    prefetch(address);
                }
                completions.push((event, None));
            });
            self.left = reaped.left;
            self.stats.reaped_max = self.stats.reaped_max.max(u64::from(reaped.count));
            if !self.lifted && reaped.count as usize == reap_cap && reaped.left {
                add(&mut self.stats.cap_hits, 1);
            }
            self.recv_reaped = self
                .completions
                .iter()
                .filter(|(event, _)| {
                    matches!(event, Completion::Recv { .. } | Completion::RecvOnce { .. })
                })
                .count();
            self.step(2);
            for at in 0..self.completions.len() {
                let event = self.completion(at).0;
                if let Some((id, op, result)) = terminal(event) {
                    let end = self.conns.ended(id, op, result);
                    if end == End::Stale {
                        add(&mut self.stats.stale_cqe, 1);
                    }
                    if end == End::Live && op == Op::Close {
                        add(&mut self.stats.close_waits, 1);
                    }
                    self.completions
                        .get_mut(at)
                        .expect("at < completions.len() in this loop")
                        .1 = Some(end);
                }
            }
            for at in 0..self.completions.len() {
                let (event, end) = self.completion(at);
                if !matches!(event, Completion::Recv { .. } | Completion::RecvOnce { .. }) {
                    self.control(event, end);
                }
            }
            self.step(3);
            for at in 0..self.candidates.len() {
                let id = *self
                    .candidates
                    .get(at)
                    .expect("at < candidates.len() in this loop");
                if self.usable(id) && !self.prepared(id) {
                    self.process_input(id);
                    if self.prepared(id) {
                        add(&mut self.stats.ready_served, 1);
                    }
                }
            }
            self.candidates.clear();
            for at in 0..self.completions.len() {
                let (event, end) = self.completion(at);
                if matches!(event, Completion::Recv { .. } | Completion::RecvOnce { .. }) {
                    self.receive(event, end);
                }
            }
            self.grants();
            self.rearm_budget = std::array::from_fn(|r| {
                self.rings
                    .free_buffers(u8::try_from(r).expect("eight rings"))
            });
            self.step(4);
            self.pending_turn();
            self.lean_turn();
            self.step(5);
            self.rearms();
            self.maintain_accept();
            self.step(6);
            #[cfg(twelve_fault_inject)]
            if !self.slept && std::fs::metadata(&self.sleep_path).is_ok() {
                self.slept = true;
                std::thread::sleep(Duration::from_secs(3));
            }
            self.discard_writes();
            if !self.batch_commit {
                self.reads();
            }
            self.step(7);
            self.step(8);
            if self.batch_commit {
                self.batch_unit();
            } else {
                self.write_window();
            }
            self.batch.retain(|work| work.flags & FINISHED == 0);
            self.step(9);
            match self.wal.step() {
                WalStep::Idle | WalStep::Slice => {}
                WalStep::Fsync => {
                    if self.sync_pending || self.sync_in_flight {
                        invariant("operation");
                    }
                    self.sync_pending = true;
                    self.push_sync();
                }
            }
            self.step(10);
            self.timers();
            self.step(11);
            self.maintain_accept();
            // No submit here: the next wait() submits every ring's queued SQEs in its own enters.
            add(&mut self.stats.turns, 1);
            self.stats.live = u64::from(self.conns.live_total());
            if self.now >= self.next_counts || self.stats.line_due(self.now) {
                self.counts();
                self.next_counts = self.now + COUNTS_PERIOD;
            }
            self.stats.emit_if_due(self.now);
            if self.now >= self.next_stats {
                self.next_stats = self.now + STATS_PERIOD;
            }
            self.busy = reaped.count != 0 || self.advanced;
            self.trim_scratch();
        }
    }

    fn step(&self, step: u8) {
        // Relaxed: this atomic publishes only its own value; no other data is read through it.
        self.progress.step.store(step, Ordering::Relaxed);
    }
    fn completion(&self, at: usize) -> (Completion, Option<End>) {
        *self
            .completions
            .get(at)
            .expect("at < completions.len() in this loop")
    }
    fn seconds(&self, now: Instant) -> u32 {
        u32::try_from(now.duration_since(self.start).as_secs())
            .unwrap_or_else(|_| invariant("clock-range"))
    }
    // usable and prepared read the connection's flags alone (Conns::flags touches no storage).
    fn usable(&self, id: ConnId) -> bool {
        self.conns
            .flags(id)
            .is_some_and(|flags| flags & flag::CLOSING == 0)
            && self
                .metadata
                .get(&self.conns, id)
                .is_none_or(|p| p.bits & CLOSE_INTENT == 0)
    }
    fn prepared(&self, id: ConnId) -> bool {
        self.metadata
            .get(&self.conns, id)
            .is_some_and(|p| p.work.flags & VALID != 0)
            || self
                .conns
                .flags(id)
                .is_some_and(|flags| flags & flag::IN_SERVICE != 0)
    }
    fn wait(&mut self) {
        let live0 = u64::from(self.conns.live(0));
        let total = u64::from(self.conns.live_total());
        // The batch target applies while completions keep coming. After a turn that reaped and
        // advanced nothing, ring 0 waits for one completion, so a lone arrival is served at once
        // and the cap below never wakes a loop that has nothing it can advance.
        let k0 = if total == 0 || !self.busy {
            1
        } else {
            u32::try_from(
                (2 * u64::from(self.batch_target) * live0)
                    .div_ceil(total)
                    .max(1),
            )
            .unwrap_or_else(|_| invariant("capacity"))
        };
        // Deadlines are absolute: work in the previous round must not extend a sleep.
        let now = Instant::now();
        let mut timeout = self
            .next_sample
            .saturating_duration_since(now)
            .min(self.next_sweep.saturating_duration_since(now))
            .min(self.next_stats.saturating_duration_since(now));
        let others_active =
            (1_u8..8).any(|ring| self.conns.live(ring) != 0 || self.rings.active(ring));
        // The ceiling, --wake-wait-us (default 4,000 us, 0 to 10,000 us), bounds how long a partial
        // batch waits on ring 0, and how long rings 1..7, whose completions cannot wake ring 0's wait,
        // go unpolled.
        if k0 > 1 || others_active {
            timeout = timeout.min(self.timeout);
        }
        let held = self.wal.writes_held(now);
        // A capped hold ends at its deadline: the loop looks at the gate again then.
        if held && let Some(deadline) = self.wal.hold_deadline() {
            timeout = timeout.min(deadline.saturating_duration_since(now));
        }
        let write_credit = self
            .writes
            .front()
            .filter(|work| self.usable(work.id))
            .is_some_and(|work| {
                let need = http::HEAD_ROOM
                    + work
                        .reply_need
                        .map_or(conn::KEEP_MAX as usize, |need| need as usize);
                // Charged output bytes never exceed OUTPUT_BUDGET and the added need never exceeds
                // WHOLE_MAX, so this sum cannot overflow.
                self.conns.output_bytes().strict_add(need as u64) <= conn::OUTPUT_BUDGET
            });
        if !held
            && write_credit
            && let Some(work) = self.writes.front()
        {
            timeout = timeout.min(
                (work.receipt + Duration::from_micros(self.window.us))
                    .saturating_duration_since(now),
            );
        }
        // Charged output bytes never exceed OUTPUT_BUDGET and WHOLE_MAX is a constant far below
        // u64::MAX, so this sum cannot overflow.
        let read_credit =
            self.conns.output_bytes().strict_add(WHOLE_MAX as u64) <= conn::OUTPUT_BUDGET;
        if self.left
            || self.wal.runnable()
            || (!self.candidates.is_empty() && self.refill)
            || (self.batch.iter().any(|work| !work.write() || !held) && read_credit)
            || (!held && self.writes.len() >= Q && write_credit)
        {
            timeout = Duration::ZERO;
        }
        match transport(self.rings.wait(k0, Some(timeout))) {
            WaitEnd::Retry => {}
            WaitEnd::Ready => {
                if !self.busy {
                    add(&mut self.stats.wake_idle, 1);
                }
            }
            WaitEnd::Count => {
                if self.busy {
                    add(&mut self.stats.wake_count, 1);
                } else {
                    add(&mut self.stats.wake_idle, 1);
                }
            }
            WaitEnd::Timeout => {
                if self.busy {
                    add(&mut self.stats.wake_timeout, 1);
                } else {
                    add(&mut self.stats.wake_idle, 1);
                }
            }
        }
    }
    fn submit(&mut self) {
        match transport(self.rings.submit_all()) {
            Submit::Done(()) | Submit::RetryNextTurn => {}
        }
    }
    fn ring_blocked(&self, ring: u8) -> bool {
        *self
            .ring_blocked
            .get(usize::from(ring))
            .expect("ring < 8: ring indices come from Completion values and the accept owner")
    }
    fn block_ring(&mut self, ring: u8) {
        *self
            .ring_blocked
            .get_mut(usize::from(ring))
            .expect("ring < 8: ring indices come from Completion values and the accept owner") =
            true;
    }
    fn push_sync(&mut self) {
        if self.ring_blocked(0) || !self.sync_pending {
            return;
        }
        match transport(self.rings.fsync(self.wal.sync_fd())) {
            Submit::Done(()) => {
                self.sync_pending = false;
                self.sync_in_flight = true;
                self.wal.sync_submitted(Instant::now());
            }
            Submit::RetryNextTurn => self.block_ring(0),
        }
    }
    fn maintain_accept(&mut self) {
        let slots = self.rings.slots();
        if self.ring_spread == crate::settings::RingSpread::Spread
            && matches!(
                self.accept,
                AcceptState::Unarmed | AcceptState::CapacityPaused
            )
        {
            if let Some(ring) = (0_u8..8)
                .filter(|&ring| self.conns.live(ring) < slots)
                .min_by_key(|&ring| (self.conns.live(ring), (ring + 8 - self.accept_owner) % 8))
            {
                self.accept_owner = ring;
                self.accept = AcceptState::Unarmed;
            } else {
                self.accept = AcceptState::CapacityPaused;
            }
        }
        let h = (slots / 10).max(1_024.min(slots / 8));
        // A ring's live count never exceeds its slots, so this difference cannot underflow.
        let free = |ring: u8, conns: &Conns| slots.strict_sub(conns.live(ring));
        if let AcceptState::Armed { ring, arm } = self.accept
            && (free(ring, &self.conns) <= h
                || (self.ring_spread == crate::settings::RingSpread::Spread
                    && self.conns.live(ring)
                        > (0_u8..8)
                            .map(|r| self.conns.live(r))
                            .min()
                            .expect("eight rings")
                            .saturating_add(self.turn_cap)))
        {
            let successor = if self.ring_spread == crate::settings::RingSpread::Spread {
                (0_u8..8)
                    .filter(|&r| free(r, &self.conns) > h)
                    .min_by_key(|&r| (self.conns.live(r), (r + 8 - ring) % 8))
            } else {
                (1..8)
                    .map(|offset| (ring + offset) % 8)
                    .find(|r| free(*r, &self.conns) > h)
            };
            self.accept = AcceptState::Cancelling {
                ring,
                arm,
                successor,
            };
            self.accept_cancel_queued = false;
        }
        // An arm after an accept error retries on its owner only while the owner is above h; a full
        // owner hands accept to the next ring above h, or pauses it until a ring reaches 2h.
        if let AcceptState::Unarmed = self.accept
            && free(self.accept_owner, &self.conns) <= h
        {
            match (1..8)
                .map(|offset| (self.accept_owner + offset) % 8)
                .find(|r| free(*r, &self.conns) > h)
            {
                Some(ring) => {
                    add(&mut self.stats.accept_handoffs, 1);
                    self.accept_owner = ring;
                }
                None => self.accept = AcceptState::CapacityPaused,
            }
        }
        if let AcceptState::CapacityPaused = self.accept
            && let Some(ring) = (0..8)
                .map(|offset| (self.accept_owner + offset) % 8)
                .find(|r| free(*r, &self.conns) >= 2 * h)
        {
            self.accept_owner = ring;
            self.accept = AcceptState::Unarmed;
        }
        if self.accept_attempted {
            return;
        }
        match self.accept {
            AcceptState::Unarmed => {
                let ring = self.accept_owner;
                if self.ring_blocked(ring) {
                    return;
                }
                self.accept_attempted = true;
                let result = self.rings.arm_accept(ring, self.listener.as_fd());
                match transport(result) {
                    Submit::Done(arm) => self.accept = AcceptState::Armed { ring, arm },
                    Submit::RetryNextTurn => self.block_ring(ring),
                }
            }
            AcceptState::Cancelling { ring, arm, .. } if !self.accept_cancel_queued => {
                if self.ring_blocked(ring) {
                    return;
                }
                self.accept_attempted = true;
                match transport(self.rings.cancel_accept(ring, arm)) {
                    Submit::Done(()) => self.accept_cancel_queued = true,
                    Submit::RetryNextTurn => self.block_ring(ring),
                }
            }
            AcceptState::Armed { .. }
            | AcceptState::Cancelling { .. }
            | AcceptState::CapacityPaused
            | AcceptState::ErrorPausedUntilSample => {}
        }
    }
    fn control(&mut self, event: Completion, end: Option<End>) {
        match event {
            Completion::Accept {
                ring,
                arm,
                result,
                more,
            } => {
                let (matches, cancelling, successor) = match self.accept {
                    AcceptState::Armed { ring: r, arm: a } => {
                        (r == ring && a == arm, false, Some(ring))
                    }
                    AcceptState::Cancelling {
                        ring: r,
                        arm: a,
                        successor,
                    } => (r == ring && a == arm, true, successor),
                    AcceptState::Unarmed
                    | AcceptState::CapacityPaused
                    | AcceptState::ErrorPausedUntilSample => (false, false, None),
                };
                if result >= 0 {
                    let slot = u16::try_from(result).unwrap_or_else(|_| invariant("operation"));
                    if matches {
                        if let Some(id) = self.conns.open(ring, slot) {
                            add(&mut self.stats.accepted, 1);
                            let peak = match ring {
                                0 => &mut self.stats.live_r0_max,
                                1 => &mut self.stats.live_r1_max,
                                2 => &mut self.stats.live_r2_max,
                                3 => &mut self.stats.live_r3_max,
                                4 => &mut self.stats.live_r4_max,
                                5 => &mut self.stats.live_r5_max,
                                6 => &mut self.stats.live_r6_max,
                                7 => &mut self.stats.live_r7_max,
                                _ => invariant("operation"),
                            };
                            *peak = (*peak).max(u64::from(self.conns.live(ring)));
                            if let Some(index) = self.conns.cell(id) {
                                self.metadata.ensure(&self.conns, id);
                                self.maybe_release(index);
                            }
                            self.schedule_arm(id, false);
                        } else {
                            add(&mut self.stats.accept_errors, 1);
                            self.orphan(ring, slot);
                        }
                    } else {
                        self.orphan(ring, slot);
                    }
                } else if !(cancelling && result == -libc::ECANCELED) {
                    add(&mut self.stats.accept_errors, 1);
                }
                if matches && !more {
                    self.accept_cancel_queued = false;
                    if result < 0 && !(cancelling && result == -libc::ECANCELED) {
                        self.accept = AcceptState::ErrorPausedUntilSample;
                    } else if let Some(next) = successor {
                        if next != self.accept_owner {
                            add(&mut self.stats.accept_handoffs, 1);
                        }
                        self.accept_owner = next;
                        self.accept = AcceptState::Unarmed;
                    } else {
                        self.accept = AcceptState::CapacityPaused;
                    }
                }
            }
            Completion::AcceptCancel { result, .. } => cancel_result(result),
            Completion::Cancel {
                ring,
                slot,
                generation,
                result,
            } => {
                cancel_result(result);
                let id = ConnId {
                    ring,
                    slot,
                    generation,
                };
                if let Some(index) = self.conns.cell(id) {
                    let cell = self.metadata.cell_mut(index);
                    if cell.id == id {
                        cell.acks = cell
                            .acks
                            .checked_sub(1)
                            .unwrap_or_else(|| invariant("operation"));
                        // The pending pass retries the arm or close this acknowledgement held back,
                        // then releases the cell once nothing is owed; a release here would drop
                        // that retry and leave a reserved connection with no receive armed.
                        self.metadata.enqueue(index);
                    } else {
                        if cell.old_generation == generation && cell.old_acks != 0 {
                            cell.old_acks -= 1;
                        }
                        self.maybe_release(index);
                    }
                }
            }
            Completion::Fsync { result } => {
                if !self.sync_in_flight {
                    invariant("operation");
                }
                self.sync_in_flight = false;
                self.wal.synced(result);
            }
            Completion::Close {
                ring,
                slot,
                generation,
                result,
            } => {
                if result != 0 {
                    runtime("close failed");
                }
                self.close_wait = self
                    .close_wait
                    .checked_sub(1)
                    .unwrap_or_else(|| invariant("operation"));
                let id = ConnId {
                    ring,
                    slot,
                    generation,
                };
                if let Some(index) = self.conns.cell(id) {
                    self.maybe_release(index);
                }
            }
            Completion::OutQ {
                ring,
                slot,
                generation,
                result,
            } => {
                if result < 0 {
                    add(&mut self.stats.outq_errors, 1);
                }
                let id = ConnId {
                    ring,
                    slot,
                    generation,
                };
                // A lean close keeps its SIOCOUTQ state in its Conns record.
                let unacked = if result == 0 {
                    Unacked::Zero
                } else {
                    Unacked::Bytes
                };
                if self.conns.lean_answer(id, unacked) {
                    return;
                }
                let Some(index) = self.conns.cell(id) else {
                    invariant("operation")
                };
                let cell = self.metadata.cell_mut(index);
                if cell.id != id || cell.bits & OUTQ_WAIT == 0 {
                    invariant("operation");
                }
                cell.bits &= !OUTQ_WAIT;
                if result == 0 {
                    cell.bits |= OUTQ_ZERO;
                }
                self.metadata.enqueue(index);
            }
            Completion::Linger { result, .. } => {
                if result < 0 {
                    add(&mut self.stats.linger_errors, 1);
                }
            }
            Completion::Shutdown {
                ring,
                slot,
                generation,
                result,
            } => {
                let id = ConnId {
                    ring,
                    slot,
                    generation,
                };
                let draining = self
                    .conns
                    .get(id)
                    .is_some_and(|conn| conn.flags & flag::DRAINING != 0);
                if result < 0 && draining {
                    self.close(id, CloseReason::Intentional);
                }
            }
            Completion::OrphanClose { ring, slot, result } => {
                if result != 0 {
                    runtime("close failed");
                }
                let id = ConnId {
                    ring,
                    slot,
                    generation: 0,
                };
                if let Some(index) = self.conns.cell(id) {
                    self.metadata.cell_mut(index).bits &= !PUSH_ORPHAN;
                    self.maybe_release(index);
                }
            }
            Completion::Send {
                ring,
                slot,
                generation,
                result,
            } => {
                let id = ConnId {
                    ring,
                    slot,
                    generation,
                };
                if end == Some(End::Live) && self.usable(id) {
                    if result <= 0 {
                        self.close(id, socket_reason(result));
                    } else {
                        self.send(id);
                        if let Some(index) = self.conns.cell(id) {
                            self.metadata.enqueue(index);
                        }
                        self.maybe_close(id);
                    }
                } else if let Some(index) = self.conns.cell(id) {
                    self.maybe_release(index);
                }
            }
            Completion::Poll {
                ring,
                slot,
                generation,
                result,
                more,
            } => {
                let id = ConnId {
                    ring,
                    slot,
                    generation,
                };
                if end == Some(End::Stale) || end == Some(End::Freed) || !self.usable(id) {
                    return;
                }
                if result > 0 {
                    add(&mut self.stats.poll_fires, 1);
                    let index = self.metadata.ensure(&self.conns, id);
                    self.metadata.cell_mut(index).bits |= POLL_READY;
                    if more {
                        self.cancel(id);
                    }
                    self.metadata.enqueue(index);
                } else if result < 0 && result != -libc::ECANCELED {
                    self.close(id, socket_reason(result));
                } else if !more {
                    self.schedule_arm(id, true);
                }
            }
            Completion::Recv { .. } | Completion::RecvOnce { .. } => invariant("operation"),
        }
    }
    fn orphan(&mut self, ring: u8, slot: u16) {
        let id = ConnId {
            ring,
            slot,
            generation: 0,
        };
        let index = if let Some(index) = self.conns.cell(id) {
            index
        } else {
            self.metadata.ensure(&self.conns, id)
        };
        if self.metadata.cell(index).bits & PUSH_ORPHAN != 0 {
            invariant("operation");
        }
        self.metadata.cell_mut(index).bits |= PUSH_ORPHAN;
        self.metadata.enqueue(index);
    }
    fn close(&mut self, id: ConnId, reason: CloseReason) {
        if !self.usable(id) {
            return;
        }
        // The close backlog counts this decided close until its Close completion arrives.
        self.close_wait = self
            .close_wait
            .checked_add(1)
            .unwrap_or_else(|| invariant("operation"));
        // With no metadata cell for this slot, no operation in flight, no request in service,
        // nothing unsent and no held ingress, the close keeps its state in the connection's own
        // record and allocates nothing.
        // UNSAFE PATH (speed): no orderly close; the fast build closes without the half-close and the read to the peer's end of stream, and resets a connection whose SIOCOUTQ answer is 0, because then every reply byte is acknowledged, and otherwise the graceful close keeps the kernel sending the unacknowledged bytes.
        if self.conns.cell(id).is_none()
            && !self.ingress.has(id)
            && self.conns.get(id).is_some_and(|conn| {
                conn.unsent == 0
                    && conn.flags
                        & (flag::RECEIVING
                            | flag::ONCE
                            | flag::POLLING
                            | flag::SENDING
                            | flag::IN_SERVICE)
                        == 0
            })
        {
            self.lean_close(id, reason);
            return;
        }
        let index = self.metadata.ensure(&self.conns, id);
        self.metadata.cell_mut(index).bits |= CLOSE_INTENT;
        self.metadata.cell_mut(index).reason = reason;
        self.metadata.enqueue(index);
    }
    // start_close's accounting and Conns work for a close kept in the connection's record.
    fn lean_close(&mut self, id: ConnId, reason: CloseReason) {
        match reason {
            CloseReason::Eof => add(&mut self.stats.closed_eof, 1),
            CloseReason::Reset => add(&mut self.stats.closed_reset, 1),
            CloseReason::Error => add(&mut self.stats.closed_error, 1),
            CloseReason::Stall => add(&mut self.stats.closed_stall, 1),
            CloseReason::Intentional => {}
        }
        if let Some(bid) = self.conns.lean_close(id) {
            self.rings.return_buffer(id.ring, bid);
        }
    }
    // Works the lean close list once a turn, after pending_turn. Reads before closes: a turn that
    // reaped any receive completion starts at most CLOSE_TRICKLE pushes; any other turn up to
    // CLOSE_BULK_RING a ring.
    fn lean_turn(&mut self) {
        let trickle = self.recv_reaped != 0;
        let mut pushes = 0;
        let mut per_ring = [0_usize; 8];
        for _ in 0..self.conns.lean_len() {
            if trickle && pushes == CLOSE_TRICKLE {
                break;
            }
            let Some((id, flags)) = self.conns.lean_pop() else {
                break;
            };
            let ring_pushes = per_ring
                .get_mut(usize::from(id.ring))
                .expect("ring < 8: a ConnId's ring is one of the eight");
            if self.ring_blocked(id.ring) || *ring_pushes == CLOSE_BULK_RING {
                self.conns.lean_requeue(id);
                continue;
            }
            // UNSAFE PATH (speed): no orderly close; the fast build closes without the half-close and the read to the peer's end of stream, and resets a connection whose SIOCOUTQ answer is 0, because then every reply byte is acknowledged, and otherwise the graceful close keeps the kernel sending the unacknowledged bytes.
            let ask = !SAFE_BUILD && flags & flag::OUTQ_DONE == 0;
            let linger = self.close_linger(flags & flag::OUTQ_ZERO != 0);
            let result = if ask {
                self.rings.outq(id.ring, id.slot, id.generation)
            } else {
                self.rings
                    .close(id.ring, id.slot, id.generation, linger, CloseIo::Quiet)
            };
            match transport(result) {
                Submit::RetryNextTurn => {
                    self.block_ring(id.ring);
                    self.conns.lean_requeue(id);
                }
                Submit::Done(()) => {
                    pushes += 1;
                    *ring_pushes += 1;
                    if ask {
                        self.conns.lean_asked(id);
                    }
                }
            }
        }
    }
    // The close's linger: an abortive close (SIOCOUTQ answered 0) keeps an inherited zero linger
    // or sets one per close; a graceful close turns an inherited zero linger off or keeps the
    // default. The safe build never asks SIOCOUTQ and always keeps the default.
    fn close_linger(&self, abortive: bool) -> CloseLinger {
        if SAFE_BUILD {
            return CloseLinger::Keep;
        }
        match (abortive, self.listener_linger) {
            (true, ListenerLinger::Inherited) | (false, ListenerLinger::PerClose) => {
                CloseLinger::Keep
            }
            (true, ListenerLinger::PerClose) => CloseLinger::Zero,
            (false, ListenerLinger::Inherited) => CloseLinger::Off,
        }
    }
    // The safe build's Intentional close, in orderly-close order: stop serving, keep receiving and
    // discard what arrives, half-close the write side, and close at the peer's end of stream.
    fn start_drain(&mut self, id: ConnId) {
        if let Some(bid) = self.conns.start_drain(id) {
            self.rings.return_buffer(id.ring, bid);
        }
        while let Some(node) = self.ingress.pop(id) {
            self.rings.return_buffer(id.ring, node.bid);
        }
        let index = self.metadata.ensure(&self.conns, id);
        self.metadata.cell_mut(index).bits |= PUSH_SHUTDOWN;
        self.metadata.enqueue(index);
    }
    fn start_close(&mut self, index: u32) {
        let id = self.metadata.cell(index).id;
        if self
            .conns
            .get(id)
            .is_none_or(|conn| conn.flags & flag::CLOSING != 0)
        {
            return;
        }
        match self.metadata.cell(index).reason {
            CloseReason::Eof => add(&mut self.stats.closed_eof, 1),
            CloseReason::Reset => add(&mut self.stats.closed_reset, 1),
            CloseReason::Error => add(&mut self.stats.closed_error, 1),
            CloseReason::Stall => add(&mut self.stats.closed_stall, 1),
            CloseReason::Intentional => {}
        }
        self.metadata.clear_request(index);
        self.metadata.cell_mut(index).bits &=
            !(PUSH_RECV | PUSH_SINGLE | PUSH_POLL | CANCEL_RECV | CANCEL_POLL | POLL_READY);
        if let Some(bid) = self.conns.close_started(id) {
            self.rings.return_buffer(id.ring, bid);
        }
        while let Some(node) = self.ingress.pop(id) {
            self.rings.return_buffer(id.ring, node.bid);
        }
        self.metadata.cell_mut(index).bits |= PUSH_CLOSE;
    }
    fn maybe_close(&mut self, id: ConnId) {
        // Both closes below need CLOSE_AFTER or READ_EOF, so any other connection is decided from
        // its flags alone.
        if self
            .conns
            .flags(id)
            .is_none_or(|flags| flags & (flag::CLOSE_AFTER | flag::READ_EOF) == 0)
            || !self.usable(id)
        {
            return;
        }
        let conn = self.conns.get(id).expect("live connection");
        if conn.flags & flag::DRAINING != 0 {
            return;
        }
        if conn.unsent == 0 && conn.flags & flag::IN_SERVICE == 0 {
            if conn.flags & flag::CLOSE_AFTER != 0 {
                // UNSAFE PATH (speed): no orderly close; the fast build closes without the half-close and the read to the peer's end of stream, and resets a connection whose SIOCOUTQ answer is 0, because then every reply byte is acknowledged, and otherwise the graceful close keeps the kernel sending the unacknowledged bytes.
                if SAFE_BUILD {
                    self.start_drain(id);
                } else {
                    self.close(id, CloseReason::Intentional);
                }
            } else if conn.flags & flag::READ_EOF != 0
                && conn.input_len == 0
                && !self.ingress.has(id)
                && !self.prepared(id)
            {
                self.close(id, CloseReason::Eof);
            }
        }
    }
    fn maybe_release(&mut self, index: u32) {
        let cell = self.metadata.cell(index);
        let id = cell.id;
        let owned = cell.bits
            & (PUSH_MASK | CLOSE_INTENT | POLL_READY | BLOCKED | OUTPUT_PAUSE | GROW_WAIT)
            != 0
            || cell.work.flags & VALID != 0
            || cell.acks != 0
            || cell.old_acks != 0
            || self.ingress.has(id);
        if !owned
            || (self.conns.flags(id).is_none()
                && cell.bits & PUSH_CLOSE == 0
                && cell.acks == 0
                && cell.old_acks == 0
                && !self.ingress.has(id))
        {
            self.metadata.release(&self.conns, index);
        }
    }
    fn cancel(&mut self, id: ConnId) {
        let Some(flags) = self.conns.flags(id) else {
            return;
        };
        let index = self.metadata.ensure(&self.conns, id);
        if self.metadata.cell(index).acks != 0
            || self.metadata.cell(index).bits & (CANCEL_RECV | CANCEL_POLL) != 0
        {
            return;
        }
        let bit = if flags & flag::RECEIVING != 0 {
            CANCEL_RECV
        } else if flags & flag::POLLING != 0 {
            CANCEL_POLL
        } else {
            return;
        };
        self.metadata.cell_mut(index).bits |= bit;
        self.metadata.enqueue(index);
    }
    fn send(&mut self, id: ConnId) {
        if !self.usable(id)
            || self
                .conns
                .flags(id)
                .is_none_or(|flags| flags & flag::SENDING != 0)
        {
            return;
        }
        if let Some((pointer, len)) = self.conns.start_send(id) {
            let index = self.metadata.ensure(&self.conns, id);
            let cell = self.metadata.cell_mut(index);
            cell.send = Some(pointer);
            cell.send_len = len;
            cell.bits |= PUSH_SEND;
            self.metadata.enqueue(index);
            self.push_slot(index);
        }
    }
    fn schedule_arm(&mut self, id: ConnId, rearm: bool) {
        // Every test before the reservation reads flags and cells alone; the full connection view
        // (Conns::get) is read once, only for a connection that may arm.
        let Some(flags) = self.conns.flags(id) else {
            return;
        };
        if flags
            & (flag::CLOSING
                | flag::READ_EOF
                | flag::PAUSED
                | flag::RECEIVING
                | flag::POLLING
                | flag::ONCE)
            != 0
            || self.ingress.has(id)
        {
            return;
        }
        let cell = self.metadata.get(&self.conns, id);
        if cell.is_some_and(|p| p.bits & CLOSE_INTENT != 0) {
            return;
        }
        let head = cell.is_some_and(|p| p.work.flags & HEAD != 0);
        let prepared =
            cell.is_some_and(|p| p.work.flags & VALID != 0) || flags & flag::IN_SERVICE != 0;
        if prepared && !head {
            return;
        }
        let conn = self.conns.get(id).expect("live connection");
        // A receive into the connection's own input reservation takes no provided buffer, so only a
        // receive that selects a provided buffer waits its turn in the ring's re-arm FIFO.
        let once = conn.input_room > 0 && (conn.flags & flag::IN_SERVICE == 0 || head);
        if !rearm
            && !once
            && (flags & flag::REARM != 0
                || (conn.input_len == 0 && conn.input_room == 0 && !self.poll_recv))
        {
            self.conns.queue_rearm(id);
            self.observe_rearm(id.ring);
            return;
        }
        let index = self.metadata.ensure(&self.conns, id);
        if self.metadata.cell(index).acks != 0 {
            self.metadata.enqueue(index);
            return;
        }
        if self.metadata.cell(index).bits & (PUSH_RECV | PUSH_SINGLE | PUSH_POLL | PUSH_ONCE) != 0 {
            return;
        }
        if rearm {
            self.metadata.cell_mut(index).bits |= REARM_COUNT;
        }
        if once {
            if let Some((pointer, len)) = self.conns.start_once(id) {
                let cell = self.metadata.cell_mut(index);
                cell.once = Some(pointer);
                cell.once_len = len;
                cell.bits |= PUSH_ONCE;
            }
        } else if conn.input_len == 0 && conn.flags & flag::IN_SERVICE == 0 {
            self.metadata.cell_mut(index).bits |= if self.poll_recv {
                if self.metadata.cell(index).bits & POLL_READY != 0 {
                    PUSH_SINGLE
                } else {
                    PUSH_POLL
                }
            } else {
                PUSH_RECV
            };
        } else {
            return;
        }
        self.metadata.enqueue(index);
        self.push_slot(index);
    }
    fn push_slot(&mut self, index: u32) {
        let id = self.metadata.cell(index).id;
        if self.ring_blocked(id.ring) {
            return;
        }
        // A cell with no push owed and no close intent pushes nothing: no kind below would act.
        if self.metadata.cell(index).bits & (PUSH_MASK | CLOSE_INTENT) == 0 {
            return;
        }
        let kinds = [
            PUSH_ONCE,
            PUSH_SEND,
            PUSH_RECV,
            PUSH_SINGLE,
            PUSH_POLL,
            CANCEL_RECV,
            CANCEL_POLL,
            PUSH_SHUTDOWN,
            PUSH_CLOSE,
            PUSH_ORPHAN,
        ];
        for kind in kinds {
            let mut bits = self.metadata.cell(index).bits;
            if bits & CLOSE_INTENT != 0 && bits & (PUSH_ONCE | PUSH_SEND | PUSH_CLOSE) == 0 {
                self.start_close(index);
                bits = self.metadata.cell(index).bits;
            }
            if bits & kind == 0 {
                continue;
            }
            let flags = self.conns.flags(id);
            if kind == PUSH_SHUTDOWN && flags.is_none_or(|flags| flags & flag::DRAINING == 0) {
                // The drain ended before its shutdown was pushed: no SQE names a closing slot.
                self.metadata.cell_mut(index).bits &= !PUSH_SHUTDOWN;
                continue;
            }
            if kind == PUSH_CLOSE && bits & OUTQ_WAIT != 0 {
                continue;
            }
            // The close's shape at this attempt: the fd-wide cancel only while an operation the
            // close must end is in flight.
            let io = if flags.is_some_and(|flags| {
                flags & (flag::RECEIVING | flag::ONCE | flag::POLLING | flag::SENDING) != 0
            }) {
                CloseIo::Pending
            } else {
                CloseIo::Quiet
            };
            // UNSAFE PATH (speed): no orderly close; the fast build closes without the half-close and the read to the peer's end of stream, and resets a connection whose SIOCOUTQ answer is 0, because then every reply byte is acknowledged, and otherwise the graceful close keeps the kernel sending the unacknowledged bytes.
            let outq = !SAFE_BUILD
                && kind == PUSH_CLOSE
                && io == CloseIo::Quiet
                && bits & OUTQ_DONE == 0
                && self.conns.get(id).is_some_and(|conn| conn.unsent == 0);
            let linger = self.close_linger(bits & OUTQ_ZERO != 0);
            if matches!(kind, PUSH_RECV | PUSH_SINGLE)
                && *self
                    .rearm_budget
                    .get(usize::from(id.ring))
                    .expect("ring < 8")
                    == 0
            {
                if self.metadata.cell(index).attempted != self.turn {
                    self.metadata.cell_mut(index).attempted = self.turn;
                    add(&mut self.stats.rearm_buffer_wait, 1);
                }
                self.metadata.enqueue(index);
                return;
            }
            let result = match kind {
                PUSH_ONCE => {
                    let cell = self.metadata.cell(index);
                    let pointer = cell.once.unwrap_or_else(|| invariant("operation"));
                    // SAFETY: recv_once needs len bytes that stay allocated, unmoved and unread
                    // until the terminal completion: start_once lent an unmoving Conns input
                    // reservation whose ONCE flag and pointer survive retries and closure intent,
                    // and only the terminal RecvOnce (or Rings::drain) ends this loan.
                    unsafe {
                        self.rings.recv_once(
                            id.ring,
                            id.slot,
                            id.generation,
                            pointer,
                            cell.once_len,
                        )
                    }
                }
                PUSH_SEND => {
                    let cell = self.metadata.cell(index);
                    let pointer = cell.send.unwrap_or_else(|| invariant("operation"));
                    // SAFETY: send needs len bytes that stay allocated, unmoved and unwritten until
                    // the terminal completion: start_send moved this allocation into Conns' flight,
                    // no render or reserve touches it, also across RetryNextTurn, and only its
                    // terminal Send (or Rings::drain) ends the loan.
                    unsafe {
                        self.rings
                            .send(id.ring, id.slot, id.generation, pointer, cell.send_len)
                    }
                }
                PUSH_RECV | PUSH_SINGLE => self.rings.arm_recv(
                    id.ring,
                    id.slot,
                    id.generation,
                    if kind == PUSH_RECV {
                        RecvMode::Multishot
                    } else {
                        RecvMode::Single
                    },
                ),
                PUSH_POLL => self.rings.arm_poll(id.ring, id.slot, id.generation),
                CANCEL_RECV | CANCEL_POLL => self.rings.cancel(
                    id.ring,
                    id.slot,
                    id.generation,
                    if kind == CANCEL_RECV {
                        Armed::Recv
                    } else {
                        Armed::Poll
                    },
                ),
                PUSH_SHUTDOWN => self.rings.shutdown(id.ring, id.slot, id.generation),
                PUSH_CLOSE if outq => self.rings.outq(id.ring, id.slot, id.generation),
                PUSH_CLOSE => self
                    .rings
                    .close(id.ring, id.slot, id.generation, linger, io),
                PUSH_ORPHAN => self.rings.close_orphan(id.ring, id.slot),
                _ => invariant("operation"),
            };
            match transport(result) {
                Submit::RetryNextTurn => {
                    self.block_ring(id.ring);
                    self.metadata.enqueue(index);
                    return;
                }
                Submit::Done(()) => {
                    if outq {
                        self.metadata.cell_mut(index).bits |= OUTQ_WAIT | OUTQ_DONE;
                        continue;
                    }
                    self.metadata.cell_mut(index).bits &= !kind;
                    match kind {
                        PUSH_ONCE => self.metadata.cell_mut(index).once = None,
                        PUSH_SEND => self.metadata.cell_mut(index).send = None,
                        PUSH_RECV | PUSH_SINGLE => {
                            let budget = self
                                .rearm_budget
                                .get_mut(usize::from(id.ring))
                                .expect("ring < 8");
                            *budget = budget.strict_sub(1);
                            self.conns.armed(id, Op::Recv);
                            self.metadata.cell_mut(index).bits &= !POLL_READY;
                        }
                        PUSH_POLL => self.conns.armed(id, Op::Poll),
                        CANCEL_RECV | CANCEL_POLL => {
                            let cell = self.metadata.cell_mut(index);
                            cell.acks = cell
                                .acks
                                .checked_add(1)
                                .unwrap_or_else(|| invariant("operation"));
                        }
                        PUSH_CLOSE => {
                            // Only an accepted close with the cancel owes its Cancel; earlier
                            // debt is kept.
                            if io == CloseIo::Pending {
                                let cell = self.metadata.cell_mut(index);
                                cell.acks = cell
                                    .acks
                                    .checked_add(1)
                                    .unwrap_or_else(|| invariant("operation"));
                            }
                            self.metadata.cell_mut(index).bits &= !(OUTQ_DONE | OUTQ_ZERO);
                        }
                        PUSH_SHUTDOWN => {}
                        PUSH_ORPHAN => {}
                        _ => invariant("operation"),
                    }
                    if matches!(kind, PUSH_ONCE | PUSH_RECV | PUSH_SINGLE | PUSH_POLL)
                        && self.metadata.cell(index).bits & REARM_COUNT != 0
                    {
                        self.metadata.cell_mut(index).bits &= !REARM_COUNT;
                        add(&mut self.stats.rearms, 1);
                    }
                }
            }
        }
    }
    fn rearms(&mut self) {
        for ring in 0..8 {
            let free = *self.rearm_budget.get(usize::from(ring)).expect("ring < 8");
            add(
                &mut self.stats.rearm_buffer_wait,
                u64::from(
                    self.conns
                        .rearm(ring)
                        .saturating_sub(free.min(self.turn_cap)),
                ),
            );
            self.candidates.clear();
            self.conns
                .take_rearm(ring, free.min(self.turn_cap), |id| self.candidates.push(id));
            for at in 0..self.candidates.len() {
                let id = *self
                    .candidates
                    .get(at)
                    .expect("at < candidates.len() in this loop");
                self.schedule_arm(id, true);
            }
        }
        self.candidates.clear();
    }
}

fn terminal(event: Completion) -> Option<(ConnId, Op, i32)> {
    let (ring, slot, generation, result, op) = match event {
        Completion::Recv {
            ring,
            slot,
            generation,
            result,
            more: false,
            ..
        } => (ring, slot, generation, result, Op::Recv),
        Completion::RecvOnce {
            ring,
            slot,
            generation,
            result,
        } => (ring, slot, generation, result, Op::Once),
        Completion::Poll {
            ring,
            slot,
            generation,
            result,
            more: false,
        } => (ring, slot, generation, result, Op::Poll),
        Completion::Send {
            ring,
            slot,
            generation,
            result,
        } => (ring, slot, generation, result, Op::Send),
        Completion::Close {
            ring,
            slot,
            generation,
            result,
        } => (ring, slot, generation, result, Op::Close),
        _ => return None,
    };
    Some((
        ConnId {
            ring,
            slot,
            generation,
        },
        op,
        result,
    ))
}
fn cancel_result(result: i32) {
    if result < 0 && result != -libc::ENOENT && result != -libc::ECANCELED {
        runtime("cancel failed");
    }
}
fn socket_reason(result: i32) -> CloseReason {
    if result == -libc::ECONNRESET || result == -libc::EPIPE {
        CloseReason::Reset
    } else {
        CloseReason::Error
    }
}

const UNCHECKED: u32 = 1 << 8;
const AUTH_PRESENT: u32 = 1 << 9;
const AUTH_CHECKED: u32 = 1 << 10;
const NAME_CACHED: u32 = 1 << 11;

impl Ingresses {
    fn push_front(&mut self, mut node: Ingress) {
        let id = ConnId {
            ring: node.ring,
            slot: node.slot,
            generation: node.generation,
        };
        let key = self.key(id);
        let index = self.node(node.ring, node.bid);
        node.next = self.head(key)[0];
        *self.entry_mut(index) = node;
        let head = self.head_mut(key);
        head[0] = node.bid + 1;
        if head[1] == 0 {
            head[1] = node.bid + 1;
        }
        if node.additional != 0 {
            // UNSAFE PATH (speed): unchecked arithmetic; held counts one ring's held bids, at most bufs_per_ring.
            *self.held_mut(node.ring) += 1;
        }
    }
}

impl Server {
    fn receive(&mut self, event: Completion, end: Option<End>) {
        let (id, result, bid, more, once) = match event {
            Completion::Recv {
                ring,
                slot,
                generation,
                result,
                buffer,
                more,
            } => (
                ConnId {
                    ring,
                    slot,
                    generation,
                },
                result,
                buffer,
                more,
                false,
            ),
            Completion::RecvOnce {
                ring,
                slot,
                generation,
                result,
            } => (
                ConnId {
                    ring,
                    slot,
                    generation,
                },
                result,
                None,
                false,
                true,
            ),
            _ => invariant("operation"),
        };
        if end == Some(End::Stale) || end == Some(End::Freed) || !self.usable(id) {
            if let Some(bid) = bid {
                self.rings.return_buffer(id.ring, bid);
            }
            if let Some(index) = self.conns.cell(id) {
                self.maybe_release(index);
            }
            return;
        }
        let draining = self
            .conns
            .flags(id)
            .is_some_and(|flags| flags & flag::DRAINING != 0);
        if draining {
            self.drain_receive(id, result, bid);
        } else if result > 0 {
            if once {
                self.process_input(id);
            } else {
                let bid = bid.unwrap_or_else(|| invariant("ingress"));
                // UNSAFE PATH (speed): unchecked narrowing casts; a positive receive result is at most the provided buffer's ring::BUF_SIZE (2,048) bytes, checked just above, so it fits u32 and u16 exactly.
                #[expect(
                    clippy::cast_sign_loss,
                    reason = "result > 0 in this branch, so the fast build's cast keeps its value"
                )]
                let count = if SAFE_BUILD {
                    u32::try_from(result).unwrap_or_else(|_| invariant("ingress"))
                } else {
                    result as u32
                };
                if count > ring::BUF_SIZE {
                    invariant("ingress");
                }
                // UNSAFE PATH (speed): unchecked narrowing casts; a positive receive result is at most the provided buffer's ring::BUF_SIZE (2,048) bytes, checked just above, so it fits u32 and u16 exactly.
                #[expect(
                    clippy::cast_possible_truncation,
                    clippy::cast_sign_loss,
                    reason = "0 < result <= BUF_SIZE here, so the fast build's cast keeps its value"
                )]
                let len = if SAFE_BUILD {
                    u16::try_from(result).unwrap_or_else(|_| invariant("ingress"))
                } else {
                    result as u16
                };
                if self.ingress.has(id)
                    || self
                        .conns
                        .flags(id)
                        .is_some_and(|flags| flags & flag::WAITING != 0)
                {
                    self.hold_bid(id, bid, len, 0, None);
                } else {
                    self.bid(id, bid, len, 0, None);
                }
            }
        } else {
            if let Some(bid) = bid {
                self.rings.return_buffer(id.ring, bid);
            }
            match result {
                0 => {
                    if self
                        .conns
                        .get(id)
                        .is_some_and(|conn| conn.flags & flag::READ_EOF == 0)
                    {
                        self.conns.read_eof(id);
                        let complete = self.prepared(id)
                            || matches!(
                                http::parse(self.conns.input(id), &mut self.chunk),
                                Parse::Complete { .. }
                            );
                        if complete {
                            let index = self.metadata.ensure(&self.conns, id);
                            if self.metadata.cell(index).bits & HALF_COUNTED == 0 {
                                self.metadata.cell_mut(index).bits |= HALF_COUNTED;
                                add(&mut self.stats.half_close, 1);
                            }
                        }
                    }
                    if !self.prepared(id) {
                        self.process_input(id);
                    }
                    self.maybe_close(id);
                }
                // The receive's end below decides where the connection waits for its next arm.
                n if n == -libc::ENOBUFS => add(&mut self.stats.enobufs, 1),
                n if n == -libc::ECANCELED => {}
                _ => self.close(id, socket_reason(result)),
            }
        }
        if !more && self.usable(id) {
            // A receive into the connection's own input reservation takes no provided buffer: such
            // a connection arms on this turn's pending pass. Only one that needs a provided buffer
            // waits in the ring's re-arm FIFO for buffer credit.
            let reserved = self.conns.get(id).is_some_and(|conn| {
                conn.input_room > 0
                    && (conn.flags & flag::IN_SERVICE == 0
                        || self
                            .metadata
                            .get(&self.conns, id)
                            .is_some_and(|p| p.work.flags & HEAD != 0))
            });
            if reserved {
                let index = self.metadata.ensure(&self.conns, id);
                self.metadata.enqueue(index);
            } else {
                if !once {
                    self.conns.queue_rearm(id);
                    self.observe_rearm(id.ring);
                }
                if let Some(index) = self.conns.cell(id) {
                    self.metadata.enqueue(index);
                }
            }
        }
    }
    // The safe build's drain parses nothing: a received buffer goes back unread, a one-shot
    // receive's bytes went with the input storage conn released at its end, and the peer's end of
    // stream or a receive error closes the connection.
    fn drain_receive(&mut self, id: ConnId, result: i32, bid: Option<u16>) {
        if let Some(bid) = bid {
            self.rings.return_buffer(id.ring, bid);
        }
        match result {
            n if n > 0 => {}
            n if n == -libc::ENOBUFS => {
                add(&mut self.stats.enobufs, 1);
                self.conns.queue_rearm(id);
                self.observe_rearm(id.ring);
            }
            n if n == -libc::ECANCELED => {}
            _ => self.close(id, CloseReason::Intentional),
        }
    }

    fn request_work(&mut self, id: ConnId, request: Request) -> Work {
        // The receipt is the turn's clock reading: every request of a turn is received after it.
        let mut work = Work::empty(self.now);
        work.id = id;
        work.route = request.route;
        work.close = request.close;
        work.version = request.version;
        work.flags = VALID | UNCHECKED;
        work.name = request.authorization.unwrap_or(Span { start: 0, len: 0 });
        if request.authorization.is_some() {
            work.flags |= AUTH_PRESENT;
        }
        self.ordinal = self
            .ordinal
            .checked_add(1)
            .unwrap_or_else(|| invariant("receipt-order"));
        work.ordinal = self.ordinal;
        if request.version == Version::Http10 {
            add(&mut self.stats.http10_requests, 1);
        }
        work
    }
    fn stage_complete(&mut self, id: ConnId, request: Request, consumed: usize, retained: bool) {
        let index = self.metadata.ensure(&self.conns, id);
        let cached = self.metadata.cell(index).work.flags & HEAD != 0;
        let mut work = if cached {
            self.metadata.cell(index).work
        } else {
            self.request_work(id, request)
        };
        work.flags &= !HEAD;
        work.consumed = u32n(consumed, "framing");
        work.held = if retained { work.consumed } else { 0 };
        work.route = request.route;
        work.close = request.close;
        work.body = match request.body {
            Body::Empty => Span { start: 0, len: 0 },
            Body::Input(span) => span,
            Body::Chunked { at, len } => {
                if !retained || self.chunk.len() != len as usize {
                    invariant("framing");
                }
                range(Span { start: at, len }, consumed);
                self.conns.rewrite_input(id, at, &self.chunk);
                add(&mut self.stats.chunked_bodies, 1);
                Span { start: at, len }
            }
            Body::Pending => invariant("framing"),
        };
        if cached {
            work.flags |= UNCHECKED;
        }
        self.metadata.cell_mut(index).work = work;
        self.metadata.enqueue(index);
    }
    fn stage_framing(&mut self, id: ConnId, retained: bool) {
        add(&mut self.stats.framing_errors, 1);
        let request = Request {
            route: Route::NotFound,
            authorization: None,
            body: Body::Empty,
            close: true,
            version: Version::Http11,
        };
        let index = self.metadata.ensure(&self.conns, id);
        let mut work = self.request_work(id, request);
        work.flags = VALID | EARLY;
        work.error = Some(ErrorBody::Internal);
        work.status = Status::Internal;
        if retained {
            work.held = self.conns.get(id).expect("live connection").input_len;
            work.consumed = work.held;
        }
        self.metadata.cell_mut(index).work = work;
        self.metadata.enqueue(index);
        self.cancel(id);
    }
    fn process_input(&mut self, id: ConnId) {
        self.process_parsed(id, None);
    }
    // process_input with the Parse of the connection's input when the caller already holds it: bid
    // passes the parse of a whole request it has just moved, byte for byte, into the empty input.
    fn process_parsed(&mut self, id: ConnId, parsed: Option<Parse>) {
        if !self.usable(id) {
            return;
        }
        let conn = self.conns.get(id).expect("live connection");
        if conn.flags & (flag::ONCE | flag::WAITING) != 0 {
            return;
        }
        let (checked_head, growing, valid) =
            self.metadata
                .get(&self.conns, id)
                .map_or((false, false, false), |p| {
                    (
                        p.work.flags & HEAD != 0,
                        p.bits & GROW_WAIT != 0,
                        p.work.flags & VALID != 0,
                    )
                });
        if (valid || conn.flags & flag::IN_SERVICE != 0) && !checked_head {
            return;
        }
        if conn.input_len == 0 {
            self.maybe_close(id);
            return;
        }
        let parse = match parsed {
            Some(parse) => parse,
            None => http::parse(self.conns.input(id), &mut self.chunk),
        };
        // A growth wait ends at this parse unless the reservation still cannot grow below.
        if growing {
            let index = self.metadata.ensure(&self.conns, id);
            self.metadata.cell_mut(index).bits &= !GROW_WAIT;
        }
        match parse {
            Parse::Incomplete { need } => {
                if conn.flags & flag::READ_EOF != 0 {
                    self.close(id, CloseReason::Eof);
                    return;
                }
                let want = need.max(conn.input_len);
                if !self.reserve_input(id, want) {
                    self.grow_wait(id);
                }
                self.cancel(id);
                let index = self.metadata.ensure(&self.conns, id);
                self.metadata.enqueue(index);
            }
            Parse::Continue { request, need } => {
                if conn.flags & flag::READ_EOF != 0 {
                    self.close(id, CloseReason::Eof);
                    return;
                }
                // With input credit short the head waits like any request waiting for credit, and
                // its route and auth check runs once, when its reservation is granted.
                if !self.reserve_input(id, need.max(conn.input_len)) {
                    self.grow_wait(id);
                    self.cancel(id);
                    return;
                }
                if !checked_head {
                    let mut work = self.request_work(id, request);
                    work.flags |= HEAD;
                    work.need = need;
                    let index = self.metadata.ensure(&self.conns, id);
                    self.metadata.cell_mut(index).work = work;
                    self.metadata.enqueue(index);
                }
                self.cancel(id);
            }
            Parse::Complete { request, consumed } => {
                self.stage_complete(id, request, consumed, true)
            }
            Parse::Error { close: true } => self.stage_framing(id, true),
            Parse::Error { close: false } => invariant("framing"),
        }
    }
    // A refused growth of a partial request's reservation: conn revokes no granted capacity, so the
    // request keeps its reservation, its bytes and their order, counted once in input_full, and
    // waits for input credit; bytes past the reservation stay in their provided buffers.
    fn grow_wait(&mut self, id: ConnId) {
        let index = self.metadata.ensure(&self.conns, id);
        let cell = self.metadata.cell_mut(index);
        cell.bits |= GROW_WAIT;
        if cell.bits & INPUT_COUNTED == 0 {
            cell.bits |= INPUT_COUNTED;
            add(&mut self.stats.input_full, 1);
        }
        self.metadata.enqueue(index);
    }

    fn bid(&mut self, id: ConnId, bid: u16, len: u16, mut offset: u16, prior: Option<Ingress>) {
        if !self.usable(id) {
            self.rings.return_buffer(id.ring, bid);
            return;
        }
        if u32::from(len) > ring::BUF_SIZE || offset > len {
            invariant("ingress");
        }
        let conn = self.conns.get(id).expect("live connection");
        if conn.flags & (flag::ONCE | flag::WAITING) != 0 {
            self.hold_bid(id, bid, len, offset, prior);
            return;
        }
        // A write (or a Continue head, or a chunked body) whole in this buffer is parsed once: its
        // Parse is kept and staged after the bytes move into the connection's input.
        let mut whole = None;
        let valid = self
            .metadata
            .get(&self.conns, id)
            .is_some_and(|p| p.work.flags & VALID != 0);
        if conn.input_len == 0
            && conn.input_room == 0
            && !valid
            && conn.flags & flag::IN_SERVICE == 0
        {
            let source = self
                .rings
                .buffer(id.ring, bid, u32::from(len))
                .get(usize::from(offset)..)
                .expect("offset <= len, checked above");
            let parse = http::parse(source, &mut self.chunk);
            match parse {
                Parse::Complete { request, consumed }
                    if !matches!(
                        request.route,
                        Route::Create | Route::Like(_) | Route::InvalidId { auth: true }
                    ) && !matches!(request.body, Body::Chunked { .. }) =>
                {
                    let direct = matches!(request.route, Route::Health | Route::Feed | Route::Post(_))
                            && self.refill
                            && self.batch.len() < self.cap
                            && self.conns.cell(id).is_none()
                            && conn.unsent < conn::UNSENT_MAX
                            // Admission only observes credit; reads() reserves it once at execution.
                            && self.conns.output_bytes().strict_add(WHOLE_MAX as u64)
                                <= conn::OUTPUT_BUDGET;
                    if direct {
                        let mut work = self.request_work(id, request);
                        work.flags &= !UNCHECKED;
                        work.consumed = u32n(consumed, "framing");
                        self.conns.begin_service(id);
                        self.batch.push(work);
                        self.advanced = true;
                    } else {
                        self.stage_complete(id, request, consumed, false);
                    }
                    offset = offset
                        .checked_add(
                            u16::try_from(consumed).unwrap_or_else(|_| invariant("framing")),
                        )
                        .unwrap_or_else(|| invariant("framing"));
                }
                Parse::Error { close: true } => {
                    self.stage_framing(id, false);
                    self.rings.return_buffer(id.ring, bid);
                    return;
                }
                Parse::Error { close: false } => invariant("framing"),
                _ => {
                    let want = match parse {
                        Parse::Incomplete { need } | Parse::Continue { need, .. } => need,
                        Parse::Complete { request, consumed } => {
                            if !matches!(request.body, Body::Chunked { .. }) {
                                whole = Some(parse);
                            }
                            u32n(consumed, "framing")
                        }
                        Parse::Error { .. } => invariant("framing"),
                    };
                    let grantable = self.conns.waiting() == 0
                        // UNSAFE PATH (speed): unchecked arithmetic; charged bytes never exceed their budget and the addend never exceeds INPUT_MAX or WHOLE_MAX, far below u64::MAX.
                        && self.conns.input_bytes() + u64::from(want) <= conn::INPUT_BUDGET;
                    if let Parse::Continue { request, need } = parse
                        && grantable
                    {
                        let mut work = self.request_work(id, request);
                        work.flags |= HEAD;
                        work.need = need;
                        let source = self
                            .rings
                            .buffer(id.ring, bid, u32::from(len))
                            .get(usize::from(offset)..)
                            .expect("offset <= len, checked above");
                        route_auth(
                            &self.auth,
                            &mut self.claims,
                            &mut self.stats,
                            &mut self.wall,
                            source,
                            &mut work,
                        );
                        let index = self.metadata.ensure(&self.conns, id);
                        if work.error.is_some() {
                            work.flags &= !(HEAD | UNCHECKED);
                            work.flags |= EARLY;
                            work.close = true;
                            self.metadata.cell_mut(index).work = work;
                            self.metadata.enqueue(index);
                            self.rings.return_buffer(id.ring, bid);
                            return;
                        }
                        if !self.reserve_input(id, want) {
                            invariant("capacity");
                        }
                        if work.flags & AUTH_CHECKED != 0 {
                            let name = self
                                .claims
                                .username()
                                .unwrap_or_else(|| invariant("claims"));
                            self.metadata.save_name(index, name.as_bytes());
                            work.flags |= NAME_CACHED;
                        }
                        self.metadata.cell_mut(index).work = work;
                        self.metadata.enqueue(index);
                    } else if !self.reserve_input(id, want) {
                        self.input_wait(id, bid, len, offset, want);
                        return;
                    }
                }
            }
        }
        if offset < len {
            let conn = self.conns.get(id).expect("live connection");
            let room = if conn.input_len == 0 && conn.input_room == 0 {
                let want = u32::from(len - offset);
                if !self.reserve_input(id, want) {
                    self.input_wait(id, bid, len, offset, want);
                    return;
                }
                self.conns.get(id).expect("live connection").input_room
            } else {
                conn.input_room
            };
            // UNSAFE PATH (speed): unchecked narrowing casts; a positive receive result is at most the provided buffer's ring::BUF_SIZE (2,048) bytes, checked just above, so it fits u32 and u16 exactly.
            #[expect(
                clippy::cast_possible_truncation,
                reason = "the count is at most len - offset <= BUF_SIZE, so the fast build's cast keeps its value"
            )]
            let count = if SAFE_BUILD {
                u16::try_from(room.min(u32::from(len - offset)))
                    .unwrap_or_else(|_| invariant("ingress"))
            } else {
                room.min(u32::from(len - offset)) as u16
            };
            if count != 0 {
                let bytes = self
                    .rings
                    .buffer(id.ring, bid, u32::from(len))
                    .get(usize::from(offset)..usize::from(offset + count))
                    .expect("offset + count <= len: count is at most len - offset");
                if !self.conns.append_input(id, bytes) {
                    invariant("ingress");
                }
                offset += count;
            }
            let head = self
                .metadata
                .get(&self.conns, id)
                .is_some_and(|p| p.work.flags & HEAD != 0);
            if !self.prepared(id) || head {
                self.process_parsed(id, whole);
            } else {
                self.pipeline(id);
            }
        }
        if offset == len {
            self.rings.return_buffer(id.ring, bid);
        } else {
            self.hold_bid(id, bid, len, offset, prior);
        }
    }
    fn pipeline(&mut self, id: ConnId) {
        let index = self.metadata.ensure(&self.conns, id);
        if self.metadata.cell(index).bits & PIPE_COUNTED == 0 {
            self.metadata.cell_mut(index).bits |= PIPE_COUNTED;
            add(&mut self.stats.pipelined_waits, 1);
        }
    }
    fn input_wait(&mut self, id: ConnId, bid: u16, len: u16, offset: u16, want: u32) {
        let index = self.metadata.ensure(&self.conns, id);
        if self.metadata.cell(index).bits & INPUT_COUNTED == 0 {
            self.metadata.cell_mut(index).bits |= INPUT_COUNTED;
            add(&mut self.stats.input_full, 1);
        }
        let node = self.ingress.node(id.ring, bid);
        self.ingress.entry_mut(node).offset = offset;
        self.conns.pause(id, Pause::Input { want, bid, len });
        self.cancel(id);
    }
    fn hold_bid(&mut self, id: ConnId, bid: u16, len: u16, offset: u16, prior: Option<Ingress>) {
        if let Some(mut node) = prior {
            node.offset = offset;
            self.ingress.push_front(node);
        } else {
            self.ingress
                .insert(id, bid, len, offset, false, &mut self.stats);
        }
        let index = self.metadata.ensure(&self.conns, id);
        self.metadata.enqueue(index);
        self.cancel(id);
    }
    fn grants(&mut self) {
        self.candidates.clear();
        let (ingress, stats, candidates) =
            (&mut self.ingress, &mut self.stats, &mut self.candidates);
        self.conns.take_granted(|id, bid, len| {
            let at = ingress.node(id.ring, bid);
            let offset = ingress.entry(at).offset;
            ingress.entry_mut(at).offset = 0;
            ingress.insert(id, bid, len, offset, true, stats);
            candidates.push(id);
        });
        for id in self.candidates.drain(..) {
            let index = self.metadata.ensure(&self.conns, id);
            self.metadata.enqueue(index);
        }
        self.stats.input_bytes_max = self.stats.input_bytes_max.max(self.conns.input_bytes());
    }
    fn pending_turn(&mut self) {
        self.push_sync();
        // A lifted turn pushes every queued entry; otherwise the turn cap applies.
        let count = if self.lifted {
            self.metadata.queued
        } else {
            self.metadata.queued.min(self.cap)
        };
        let mut index = self.metadata.first;
        for _ in 0..count {
            if index == NONE {
                break;
            }
            let next = self.metadata.cell(index).next;
            let id = self.metadata.cell(index).id;
            self.metadata.unlink(index);
            self.push_slot(index);
            if self.usable(id) {
                let flags = self.conns.flags(id).expect("live connection");
                if flags & (flag::ONCE | flag::WAITING) == 0
                    && let Some(node) = self.ingress.pop(id)
                {
                    self.bid(id, node.bid, node.len, node.offset, Some(node));
                } else if self.metadata.cell(index).bits & GROW_WAIT != 0
                    && self.conns.waiting() == 0
                {
                    // A refused growth is retried once a turn; conn grants no growth while an input
                    // waiter queues.
                    self.process_input(id);
                }
                if self.usable(id) && self.metadata.cell(index).work.flags & VALID != 0 {
                    self.admit(index);
                }
                if self.usable(id) {
                    if self.metadata.cell(index).bits & OUTPUT_PAUSE != 0 {
                        let conn = self.conns.get(id).expect("live connection");
                        if conn.unsent < conn::UNSENT_MAX / 2
                            && self.metadata.cell(index).work.flags & VALID == 0
                            && conn.flags & flag::WAITING == 0
                        {
                            self.metadata.cell_mut(index).bits &= !OUTPUT_PAUSE;
                            // UNSAFE PATH (speed): unchecked arithmetic; output_paused counts the cells holding OUTPUT_PAUSE, at most 8 * slots.
                            self.metadata.output_paused -= 1;
                            if conn.flags & flag::PAUSED != 0 {
                                self.resume(id);
                            }
                        }
                    }
                    self.schedule_arm(id, false);
                    self.maybe_close(id);
                }
            }
            let cell = self.metadata.cell(index);
            let keep = cell.bits & (PUSH_MASK | CLOSE_INTENT | BLOCKED | OUTPUT_PAUSE | GROW_WAIT)
                != 0
                || cell.work.flags & VALID != 0
                || self.ingress.has(id)
                || (cell.acks != 0
                    && self
                        .conns
                        .flags(id)
                        .is_some_and(|flags| flags & flag::CLOSING == 0));
            if keep {
                self.metadata.enqueue(index);
            } else {
                self.maybe_release(index);
            }
            index = if next == NONE {
                self.metadata.first
            } else {
                next
            };
        }
    }
    fn validate(&mut self, index: u32) {
        let id = self.metadata.cell(index).id;
        let mut work = self.metadata.cell(index).work;
        if work.flags & UNCHECKED == 0 {
            return;
        }
        if work.flags & AUTH_CHECKED == 0 {
            route_auth(
                &self.auth,
                &mut self.claims,
                &mut self.stats,
                &mut self.wall,
                self.conns.input(id),
                &mut work,
            );
        }
        if work.error.is_none() && work.flags & HEAD == 0 && matches!(work.route, Route::Create) {
            let input = self.conns.input(id);
            let raw = input
                .get(range(work.body, input.len()))
                .unwrap_or_else(|| invariant("framing"));
            match json::parse_body(raw, &mut self.unescape) {
                Err(error) => {
                    work.error = Some(match error {
                        BodyError::Malformed => ErrorBody::MalformedJson,
                        BodyError::Required => ErrorBody::BodyRequired,
                        BodyError::TooLong => ErrorBody::BodyTooLong,
                    })
                }
                Ok(body) => {
                    let source = body.as_ptr().addr();
                    let base = raw.as_ptr().addr();
                    if source >= base
                        && source
                            .checked_add(body.len())
                            .is_some_and(|end| end <= base + raw.len())
                    {
                        work.body.start += u32n(source - base, "framing");
                        work.body.len = u32n(body.len(), "framing");
                    } else {
                        let len = body.len();
                        let scratch = self.unescape.as_ptr().addr();
                        let selected = source
                            .checked_sub(scratch)
                            .unwrap_or_else(|| invariant("framing"));
                        range(
                            Span {
                                start: u32n(selected, "framing"),
                                len: u32n(len, "framing"),
                            },
                            self.unescape.len(),
                        );
                        let decoded = self
                            .unescape
                            .get(selected..selected + len)
                            .unwrap_or_else(|| invariant("framing"));
                        self.conns.rewrite_input(id, work.body.start, decoded);
                        work.body.len = u32n(len, "framing");
                    }
                }
            }
        }
        if work.error.is_none() && work.flags & AUTH_CHECKED != 0 {
            if work.flags & HEAD != 0 {
                if work.flags & NAME_CACHED == 0 {
                    let name = self
                        .claims
                        .username()
                        .unwrap_or_else(|| invariant("claims"));
                    self.metadata.save_name(index, name.as_bytes());
                    work.flags |= NAME_CACHED;
                }
            } else {
                let cached = work.flags & NAME_CACHED != 0;
                let name = if cached {
                    self.metadata.cell(index).name.as_slice()
                } else {
                    self.claims
                        .username()
                        .unwrap_or_else(|| invariant("claims"))
                        .as_bytes()
                };
                if name.len() > work.name.len as usize {
                    invariant("claims");
                }
                self.conns.rewrite_input(id, work.name.start, name);
                work.name.len = u32n(name.len(), "claims");
            }
        }
        work.flags &= !UNCHECKED;
        if let Some(error) = work.error {
            work.flags |= EARLY;
            work.status = error.status();
            if work.flags & HEAD != 0 {
                work.close = true;
                work.flags &= !HEAD;
            }
        }
        self.metadata.cell_mut(index).work = work;
    }
    fn blocked(&mut self, index: u32, output: bool, queue: bool) {
        let id = self.metadata.cell(index).id;
        let cell = self.metadata.cell_mut(index);
        cell.bits |= BLOCKED;
        if output && cell.bits & OUTPUT_COUNTED == 0 {
            cell.bits |= OUTPUT_COUNTED;
            add(&mut self.stats.output_full, 1);
        }
        if queue && cell.bits & QUEUE_COUNTED == 0 {
            cell.bits |= QUEUE_COUNTED;
            add(&mut self.stats.write_queue_full, 1);
        }
        // Only output pressure pauses the connection and cancels its receive: there the client is the
        // one not reading. A request blocked by a full read batch or write queue stays in its cell with
        // its receive armed and is admitted on a later turn; bytes a client pipelines behind it are
        // held by hold_bid, as behind a request in service.
        if output {
            if cell.bits & OUTPUT_PAUSE == 0 {
                cell.bits |= OUTPUT_PAUSE;
                // UNSAFE PATH (speed): unchecked arithmetic; output_paused counts the cells holding OUTPUT_PAUSE, at most 8 * slots.
                self.metadata.output_paused += 1;
                self.stats.paused_conns_max =
                    self.stats.paused_conns_max.max(self.metadata.output_paused);
            }
            self.conns.pause(id, Pause::Output);
            self.cancel(id);
        }
        self.metadata.enqueue(index);
    }
    fn admit(&mut self, index: u32) {
        let id = self.metadata.cell(index).id;
        self.validate(index);
        if !self.usable(id) {
            return;
        }
        if !self.refill {
            self.metadata.enqueue(index);
            return;
        }
        let conn = self.conns.get(id).expect("live connection");
        let paused = self.metadata.cell(index).bits & OUTPUT_PAUSE != 0;
        if conn.unsent
            >= if paused {
                conn::UNSENT_MAX / 2
            } else {
                conn::UNSENT_MAX
            }
        {
            self.blocked(index, true, false);
            return;
        }
        let is_head = self.metadata.cell(index).work.flags & HEAD != 0;
        if is_head {
            if conn.flags & flag::CONTINUE_SENT == 0 {
                if !self.reserve_output(id, 25) {
                    self.blocked(index, true, false);
                    return;
                }
                if conn.flags & flag::IN_SERVICE == 0 {
                    self.conns.begin_service(id);
                }
                let base = u32n(self.conns.output(id).len(), "capacity");
                http::write_continue(self.conns.output(id));
                self.conns.commit_output(id, base, base, Reply::Continue);
                add(&mut self.stats.continues, 1);
                self.send(id);
            }
            self.metadata.cell_mut(index).bits &= !BLOCKED;
            if self.metadata.cell(index).bits & OUTPUT_PAUSE != 0 {
                self.metadata.cell_mut(index).bits &= !OUTPUT_PAUSE;
                // UNSAFE PATH (speed): unchecked arithmetic; output_paused counts the cells holding OUTPUT_PAUSE, at most 8 * slots.
                self.metadata.output_paused -= 1;
                if conn.flags & flag::PAUSED != 0 {
                    self.resume(id);
                }
            }
            return;
        }
        let early = self.metadata.cell(index).work.flags & EARLY != 0;
        let write = self.metadata.cell(index).work.write();
        if !early && !self.batch_commit && write && self.writes.len() == WRITE_QUEUE {
            self.blocked(index, false, true);
            return;
        }
        if !early && (self.batch_commit || !write) && self.batch.len() == self.cap {
            self.blocked(index, false, false);
            return;
        }
        if early && !self.reserve_output(id, conn::KEEP_MAX) {
            self.blocked(index, true, false);
            return;
        }
        if conn.flags & flag::IN_SERVICE == 0 {
            self.conns.begin_service(id);
        }
        if self.metadata.cell(index).bits & OUTPUT_PAUSE != 0 {
            self.metadata.cell_mut(index).bits &= !OUTPUT_PAUSE;
            // UNSAFE PATH (speed): unchecked arithmetic; output_paused counts the cells holding OUTPUT_PAUSE, at most 8 * slots.
            self.metadata.output_paused -= 1;
            if conn.flags & flag::WAITING == 0 && conn.flags & flag::PAUSED != 0 {
                self.resume(id);
            }
        }
        self.metadata.cell_mut(index).bits &= !BLOCKED;
        let mut work = self.metadata.cell(index).work;
        // The request leaves its cell for a queue, which empties the cell's work (clear_request).
        self.metadata.cell_mut(index).work.flags = 0;
        if early {
            let error = work.error.unwrap_or_else(|| invariant("framing"));
            self.render_error(&mut work, error);
            self.release_reply(&mut work);
        } else {
            if write {
                // The reply's size, and with it the UTF-8 check of the name and body, is computed
                // once, here; the window, its credit and the store read the stored value.
                work.reply_need = self.write_need(&work).map(|need| u32n(need, "capacity"));
            }
            if !self.batch_commit && write {
                self.writes.push_back(work);
                self.write_joined = true;
                self.stats.write_queue_max =
                    self.stats.write_queue_max.max(self.writes.len() as u64);
            } else {
                self.batch.push(work);
            }
        }
        self.advanced = true;
    }
}

// wall caches the turn's wall clock: token expiry is checked against a reading at most one turn old.
fn route_auth(
    auth: &Auth,
    claims: &mut Claims,
    stats: &mut Stats,
    wall: &mut Option<i64>,
    input: &[u8],
    work: &mut Work,
) {
    let needs_auth = matches!(
        work.route,
        Route::Create | Route::Like(_) | Route::InvalidId { auth: true }
    );
    if needs_auth && work.flags & AUTH_CHECKED == 0 {
        let header = if work.flags & AUTH_PRESENT != 0 {
            Some(
                input
                    .get(range(work.name, input.len()))
                    .unwrap_or_else(|| invariant("framing")),
            )
        } else {
            None
        };
        match auth.verify(header, *wall.get_or_insert_with(unix_seconds), claims) {
            Ok(()) => {
                add(&mut stats.auth_ok, 1);
                work.user_id = claims.user_id().unwrap_or_else(|| invariant("claims"));
                claims.username().unwrap_or_else(|| invariant("claims"));
                work.flags |= AUTH_CHECKED;
            }
            Err(error) => {
                add(&mut stats.auth_fail, 1);
                work.error = Some(match error {
                    AuthError::Missing => ErrorBody::MissingBearer,
                    AuthError::Invalid => ErrorBody::InvalidToken,
                    AuthError::Payload => ErrorBody::InvalidPayload,
                });
            }
        }
    }
    if work.error.is_none() {
        work.error = match work.route {
            Route::NotFound => Some(ErrorBody::NotFound),
            Route::InvalidId { .. } => Some(ErrorBody::InvalidPostId),
            Route::Health | Route::Feed | Route::Post(_) | Route::Create | Route::Like(_) => None,
        };
    }
}

impl Server {
    fn body_start(&mut self, work: &mut Work) {
        let out = self.conns.output(work.id);
        work.base = u32n(out.len(), "capacity");
        if out.capacity() - out.len() < http::HEAD_ROOM {
            invariant("capacity");
        }
        out.resize(out.len() + http::HEAD_ROOM, 0);
        work.body_start = u32n(out.len(), "capacity");
        work.flags |= RENDERED;
    }
    fn render_error(&mut self, work: &mut Work, error: ErrorBody) {
        if work.flags & RENDERED == 0 {
            self.body_start(work);
        }
        let out = self.conns.output(work.id);
        out.truncate(work.body_start as usize);
        http::write_error(out, error);
        work.status = error.status();
        work.error = Some(error);
    }
    fn reserve_input(&mut self, id: ConnId, want: u32) -> bool {
        let admitted = self.conns.reserve_input(id, want);
        self.stats.input_bytes_max = self.stats.input_bytes_max.max(self.conns.input_bytes());
        admitted
    }
    fn reserve_output(&mut self, id: ConnId, want: u32) -> bool {
        let admitted = self.conns.reserve_output(id, want);
        self.stats.output_bytes_max = self.stats.output_bytes_max.max(self.conns.output_bytes());
        admitted
    }
    fn observe_rearm(&mut self, ring: u8) {
        let queued = self.conns.rearm(ring);
        self.stats.rearm_max = self.stats.rearm_max.max(u64::from(queued));
    }
    fn resume(&mut self, id: ConnId) {
        self.conns.resume(id);
        self.observe_rearm(id.ring);
    }
    fn read_credit(&mut self, id: ConnId) -> bool {
        // UNSAFE PATH (speed): unchecked arithmetic; charged bytes never exceed their budget and the addend never exceeds INPUT_MAX or WHOLE_MAX, far below u64::MAX.
        if self.conns.output_bytes() + WHOLE_MAX as u64 > conn::OUTPUT_BUDGET {
            return false;
        }
        if !self.reserve_output(id, conn::KEEP_MAX) {
            return false;
        }
        let spare = {
            let output = self.conns.output(id);
            output.capacity() - output.len()
        };
        // UNSAFE PATH (speed): unchecked arithmetic; charged bytes never exceed their budget and the addend never exceeds INPUT_MAX or WHOLE_MAX, far below u64::MAX.
        if self.conns.output_bytes() + WHOLE_MAX.saturating_sub(spare) as u64 > conn::OUTPUT_BUDGET
        {
            invariant("capacity");
        }
        true
    }
    fn write_need(&self, work: &Work) -> Option<usize> {
        match work.route {
            Route::Create => {
                let input = self.conns.input(work.id);
                Store::create_need(text(input, work.name), text(input, work.body))
            }
            Route::Like(id) => Some(Store::like_need(id)),
            _ => invariant("framing"),
        }
    }
    fn write_credit(&mut self, work: &Work) -> bool {
        let need = work
            .reply_need
            .map_or(conn::KEEP_MAX as usize, |need| need as usize);
        self.reserve_output(work.id, u32n(http::HEAD_ROOM + need, "capacity"))
    }
    fn execute_read(&mut self, work: &mut Work) -> bool {
        if work.flags & EXECUTED == 0 {
            match work.route {
                Route::Feed => add(&mut self.stats.feeds, 1),
                Route::Post(_) => add(&mut self.stats.posts, 1),
                Route::Health => add(&mut self.stats.healths, 1),
                _ => invariant("framing"),
            }
            work.flags |= EXECUTED;
        }
        self.body_start(work);
        if matches!(work.route, Route::Health) {
            let result = self.store.health();
            let mut output = std::mem::take(self.conns.output(work.id));
            let need = match &result {
                Ok(()) => 64,
                Err(error) => escaped_len(error.as_bytes())
                    .and_then(|len| {
                        len.checked_add(
                            b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":}".len(),
                        )
                    })
                    .unwrap_or(usize::MAX),
            };
            *self.conns.output(work.id) = output;
            if need > BODY_MAX {
                self.render_error(work, ErrorBody::Internal);
                return !self.store.txn_alive();
            }
            if !self.reserve_output(work.id, u32n(need, "capacity")) {
                invariant("capacity");
            }
            output = std::mem::take(self.conns.output(work.id));
            match result {
                Ok(()) => {
                    output.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                    decimal(&mut output, self.start.elapsed().as_secs());
                    output.push(b'}');
                    work.status = Status::Ok;
                }
                Err(error) => {
                    output.extend_from_slice(
                        b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":",
                    );
                    json::escape_into(&mut output, error.as_bytes());
                    output.push(b'}');
                    work.status = Status::Unavailable;
                }
            }
            *self.conns.output(work.id) = output;
            return !self.store.txn_alive();
        }
        for attempt in 0..2 {
            let mut output = std::mem::take(self.conns.output(work.id));
            let result = match work.route {
                Route::Feed => self.store.feed(&mut output),
                Route::Post(id) => self.store.post(id, &mut output),
                _ => invariant("framing"),
            };
            *self.conns.output(work.id) = output;
            match result {
                Read::Ready => {
                    work.status = Status::Ok;
                    return false;
                }
                Read::NotFound => {
                    self.render_error(work, ErrorBody::PostNotFound);
                    return false;
                }
                Read::Internal => {
                    self.render_error(work, ErrorBody::Internal);
                    return false;
                }
                Read::Aborted => {
                    self.render_error(work, ErrorBody::Internal);
                    return true;
                }
                Read::Space { need } => {
                    if attempt != 0 || need > BODY_MAX {
                        invariant("capacity");
                    }
                    if !self.reserve_output(work.id, u32n(need, "capacity")) {
                        invariant("capacity");
                    }
                }
            }
        }
        invariant("capacity")
    }
    fn execute_write(&mut self, work: &mut Work) -> bool {
        if work.reply_need.is_none() {
            self.render_error(work, ErrorBody::Internal);
            return false;
        }
        self.body_start(work);
        work.flags |= EXECUTED;
        let mut output = std::mem::take(self.conns.output(work.id));
        let input = self.conns.input(work.id);
        let result = match work.route {
            Route::Create => self.store.create(
                work.user_id,
                admitted_text(input, work.name),
                admitted_text(input, work.body),
                &mut output,
            ),
            Route::Like(id) => self.store.like(work.user_id, id, &mut output),
            _ => invariant("framing"),
        };
        *self.conns.output(work.id) = output;
        match result {
            StoreWrite::Created => {
                work.status = Status::Created;
                work.flags |= MUTATED;
                false
            }
            StoreWrite::Liked { already_liked } => {
                work.status = if already_liked {
                    Status::Ok
                } else {
                    Status::Created
                };
                work.flags |= MUTATED;
                false
            }
            StoreWrite::NotFound => {
                self.render_error(work, ErrorBody::PostNotFound);
                false
            }
            StoreWrite::Internal => {
                self.render_error(work, ErrorBody::Internal);
                false
            }
            StoreWrite::Aborted => {
                self.render_error(work, ErrorBody::Internal);
                true
            }
            StoreWrite::Space { .. } => invariant("capacity"),
        }
    }
    fn release_reply(&mut self, work: &mut Work) {
        if !self.usable(work.id) {
            work.flags |= FINISHED;
            return;
        }
        let id = work.id;
        let out = self.conns.output(id);
        let start = http::write_head(out, work.body_start as usize, work.status, work.close);
        work.head_start = u32n(start, "capacity");
        if work.held != 0 {
            self.conns.consume_input(id, work.held);
        }
        self.conns.commit_output(
            id,
            work.base,
            work.head_start,
            Reply::Final { close: work.close },
        );
        if work.status.code() == 500 {
            add(&mut self.stats.replies_500, 1);
        }
        if work.flags & EARLY != 0 {
            add(&mut self.stats.early_replies, 1);
        }
        if work.close {
            add(&mut self.stats.close_after_reply, 1);
        }
        self.finals = sum(self.finals, 1);
        work.flags |= FINISHED;
        if let Some(index) = self.conns.cell(id) {
            self.metadata.clear_request(index);
        }
        self.advanced = true;
        self.grants();
        self.send(id);
    }
    fn rollback(&mut self) {
        if !self.store.rollback() {
            runtime("rollback failed");
        }
    }
    fn committed(&mut self) {
        if let Some(commit) = self.store.take_commit() {
            self.wal.after_commit(commit.frames, commit.started);
        }
    }
    fn batch_at(&self, at: usize) -> &Work {
        self.batch
            .get(at)
            .expect("at < batch.len(): callers loop over the batch")
    }
    fn batch_at_mut(&mut self, at: usize) -> &mut Work {
        self.batch
            .get_mut(at)
            .expect("at < batch.len(): callers loop over the batch")
    }
    fn write_at(&self, at: usize) -> &Work {
        self.writes
            .get(at)
            .expect("at < writes.len(): the window selects from the queue's head")
    }
    fn write_at_mut(&mut self, at: usize) -> &mut Work {
        self.writes
            .get_mut(at)
            .expect("at < writes.len(): the window selects from the queue's head")
    }
    // Reads the work at `at` out and leaves its slot FINISHED; a caller that keeps the work writes it
    // back. Loops read a FINISHED slot's flags alone, and the turn's retain drops it.
    fn take_batch(&mut self, at: usize) -> Work {
        let work = *self.batch_at(at);
        self.batch_at_mut(at).flags = FINISHED;
        work
    }
    fn reads(&mut self) {
        let mut began = false;
        let mut failed = false;
        let mut attempted = false;
        for kind in 0..3 {
            for at in 0..self.batch.len() {
                let selected = matches!(
                    (&self.batch_at(at).route, kind),
                    (Route::Feed, 0) | (Route::Post(_), 1) | (Route::Health, 2)
                );
                if !selected || self.batch_at(at).flags & FINISHED != 0 {
                    continue;
                }
                let mut work = self.take_batch(at);
                if !self.usable(work.id) {
                    continue;
                }
                if !self.read_credit(work.id) {
                    *self.batch_at_mut(at) = work;
                    continue;
                }
                if !attempted {
                    attempted = true;
                    began = self.store.begin_read();
                    failed = !began;
                    if failed {
                        self.rollback();
                    }
                }
                if failed {
                    self.render_error(&mut work, ErrorBody::Internal);
                } else if self.execute_read(&mut work) {
                    failed = true;
                }
                *self.batch_at_mut(at) = work;
            }
        }
        if !attempted {
            return;
        }
        let success = if began && !failed {
            let success = self.store.end_read();
            self.committed();
            success
        } else {
            false
        };
        if !success {
            self.rollback();
        }
        for at in 0..self.batch.len() {
            if self.batch_at(at).flags & RENDERED == 0 {
                continue;
            }
            let mut work = self.take_batch(at);
            if !success && work.status == Status::Ok {
                self.render_error(&mut work, ErrorBody::Internal);
            }
            self.release_reply(&mut work);
            *self.batch_at_mut(at) = work;
        }
    }
    fn discard_writes(&mut self) {
        for _ in 0..self.cap {
            if self
                .writes
                .front()
                .is_some_and(|work| !self.usable(work.id))
            {
                self.writes.pop_front();
            } else {
                break;
            }
        }
    }
    fn write_window(&mut self) {
        if self.writes.is_empty() || self.wal.writes_held(self.now) {
            return;
        }
        let now = self.now;
        let oldest = self.writes.front().expect("nonempty write queue").receipt;
        let full = self.writes.len() >= Q;
        let due = now.duration_since(oldest) >= Duration::from_micros(self.window.us);
        // Idle closes a window only on a quiet turn: one that received at most one request, or one in
        // which no write joined the queue. Under load the window or Q waiting writes close it, so a
        // commit carries the writes of several turns and writes each page they share once; a lone
        // write at low load still commits in the turn that received it.
        let quiet = self.recv_reaped <= 1 || !self.write_joined;
        let idle = quiet
            && !self.left
            && self.conns.ready() == 0
            && self.batch.iter().all(|work| work.flags & FINISHED != 0);
        if !full && !due && !idle {
            return;
        }
        let mut selected = 0;
        for at in 0..self.writes.len().min(Q) {
            let work = *self.write_at(at);
            let admitted = self.usable(work.id) && self.write_credit(&work);
            if !admitted {
                break;
            }
            selected += 1;
        }
        if selected == 0 {
            return;
        }
        add(&mut self.stats.windows, 1);
        if full {
            add(&mut self.stats.close_full, 1);
        } else if due {
            add(&mut self.stats.close_due, 1);
        } else {
            add(&mut self.stats.close_idle, 1);
        }
        let began = self.store.begin_write();
        let mut failed = !began;
        let mut attempts = 0_u64;
        for at in 0..selected {
            let mut work = *self.write_at(at);
            if failed {
                self.render_error(&mut work, ErrorBody::Internal);
            } else {
                attempts += 1;
                failed = self.execute_write(&mut work) || !self.store.txn_alive();
            }
            *self.write_at_mut(at) = work;
        }
        let success = if began && !failed {
            let success = self.store.commit();
            self.committed();
            success
        } else {
            false
        };
        if !success {
            self.rollback();
        }
        // Only the 250 ms adaptation boundary needs a fresh commit-end clock read.
        let completed = if self.window.due(self.now) {
            Instant::now()
        } else {
            self.now
        };
        self.stats.writes_per_commit_max = self.stats.writes_per_commit_max.max(attempts);
        self.stats.write_wait_us_max = self
            .stats
            .write_wait_us_max
            .max(duration_us(completed.duration_since(oldest)));
        for _ in 0..selected {
            let mut work = self.writes.pop_front().expect("selected write prefix");
            if !success && work.flags & MUTATED != 0 {
                self.render_error(&mut work, ErrorBody::Internal);
            }
            self.window.latency = sum(
                self.window.latency,
                completed.duration_since(work.receipt).as_nanos(),
            );
            self.window.writes = sum(self.window.writes, 1);
            self.release_reply(&mut work);
        }
        if self.window.due(completed) {
            self.window.adapt(completed, cpu_time(), self.finals);
        }
    }
    fn batch_unit(&mut self) {
        let held = self.wal.writes_held(self.now);
        if held {
            for at in 0..self.batch.len() {
                if self.batch_at(at).write()
                    && self.batch_at(at).flags & FINISHED == 0
                    && self.writes.len() < WRITE_QUEUE
                {
                    let work = self.take_batch(at);
                    self.writes.push_back(work);
                    self.stats.write_queue_max =
                        self.stats.write_queue_max.max(self.writes.len() as u64);
                }
            }
        }
        let mut queued = 0;
        let mut at = 0;
        let mut began = false;
        let mut failed = false;
        let mut mutation = false;
        let mut attempts = 0_u64;
        let mut executed = 0;
        while executed < self.cap {
            while self
                .batch
                .get(at)
                .is_some_and(|work| work.flags & FINISHED != 0 || (held && work.write()))
            {
                at += 1;
            }
            let old = if held {
                None
            } else {
                self.writes.get(queued).map(|work| work.ordinal)
            };
            let new = self.batch.get(at).map(|work| work.ordinal);
            let from_queue = match (old, new) {
                (None, None) => break,
                (Some(_), None) => true,
                (None, Some(_)) => false,
                (Some(old), Some(new)) => old < new,
            };
            let mut work = if from_queue {
                *self.write_at(queued)
            } else {
                self.take_batch(at)
            };
            if !self.usable(work.id) {
                if from_queue {
                    *self.write_at_mut(queued) = work;
                    queued += 1;
                } else {
                    at += 1;
                }
                continue;
            }
            let credit = if work.write() {
                self.write_credit(&work)
            } else {
                self.read_credit(work.id)
            };
            if !credit {
                if from_queue {
                    *self.write_at_mut(queued) = work;
                } else {
                    *self.batch_at_mut(at) = work;
                }
                break;
            }
            if !began && executed == 0 {
                add(&mut self.stats.batch_commits, 1);
                began = self.store.begin_batch();
                failed = !began;
            }
            if failed {
                self.render_error(&mut work, ErrorBody::Internal);
            } else if work.write() {
                attempts += 1;
                failed = self.execute_write(&mut work);
                mutation |= work.flags & MUTATED != 0;
            } else {
                if mutation {
                    work.flags |= DEPENDENT;
                }
                failed = self.execute_read(&mut work);
            }
            if began && !self.store.txn_alive() {
                failed = true;
            }
            if from_queue {
                *self.write_at_mut(queued) = work;
                queued += 1;
            } else {
                *self.batch_at_mut(at) = work;
                at += 1;
            }
            executed += 1;
            if failed {
                break;
            }
        }
        if executed == 0 {
            return;
        }
        let success = if began && !failed {
            let success = self.store.commit();
            self.committed();
            success
        } else {
            false
        };
        if !success {
            self.rollback();
        }
        self.stats.writes_per_commit_max = self.stats.writes_per_commit_max.max(attempts);
        // One reading after the commit for every write's wait.
        let completed = Instant::now();
        for _ in 0..queued {
            let mut work = self.writes.pop_front().expect("selected batch prefix");
            if work.flags & RENDERED == 0 {
                continue;
            }
            if !success && work.flags & (MUTATED | DEPENDENT) != 0 {
                self.render_error(&mut work, ErrorBody::Internal);
            }
            self.stats.write_wait_us_max = self
                .stats
                .write_wait_us_max
                .max(duration_us(completed.duration_since(work.receipt)));
            self.release_reply(&mut work);
        }
        for index in 0..self.batch.len() {
            if self.batch_at(index).flags & RENDERED == 0 {
                continue;
            }
            let mut work = self.take_batch(index);
            if !success && work.flags & (MUTATED | DEPENDENT) != 0 {
                self.render_error(&mut work, ErrorBody::Internal);
            }
            if work.write() {
                self.stats.write_wait_us_max = self
                    .stats
                    .write_wait_us_max
                    .max(duration_us(completed.duration_since(work.receipt)));
            }
            self.release_reply(&mut work);
            *self.batch_at_mut(index) = work;
        }
    }
    fn timers(&mut self) {
        let now = self.now;
        if now >= self.next_sample {
            self.next_sample = now + SAMPLE_PERIOD;
            self.wall_offset_ns = unix_nanos()
                .checked_sub(
                    i128::try_from(now.duration_since(self.start).as_nanos())
                        .unwrap_or_else(|_| invariant("clock-range")),
                )
                .unwrap_or_else(|| invariant("clock-range"));
            if matches!(self.accept, AcceptState::ErrorPausedUntilSample) {
                self.accept = AcceptState::Unarmed;
            }
        }
        if now >= self.next_sweep {
            let seconds = self.seconds(now);
            let (metadata, close_wait, conns, ingress) = (
                &mut self.metadata,
                &mut self.close_wait,
                &self.conns,
                &self.ingress,
            );
            conns.sweep(seconds, |id| {
                // A record whose wait is the server's is not stalled: with no reply byte unsent, client
                // bytes the server holds unprocessed or a complete request prepared or in service. Unsent
                // bytes wait for the client's reads, and a checked Expect head for the client's body. A
                // refused reservation growth stays under the sweep, which breaks a credit deadlock among
                // such waiters.
                let pending = metadata.get(conns, id);
                let growing = pending.is_some_and(|p| p.bits & GROW_WAIT != 0);
                let request = pending.map(|p| p.work.flags & (VALID | HEAD));
                let server_wait = !growing
                    && conns.get(id).is_some_and(|conn| {
                        conn.unsent == 0
                            && (ingress.has(id)
                                || match request {
                                    Some(flags) if flags & HEAD != 0 => false,
                                    Some(flags) if flags & VALID != 0 => true,
                                    _ => conn.flags & flag::IN_SERVICE != 0,
                                })
                    });
                if server_wait {
                    return;
                }
                let index = metadata.ensure(conns, id);
                if metadata.cell(index).bits & CLOSE_INTENT == 0 {
                    metadata.cell_mut(index).bits |= CLOSE_INTENT;
                    metadata.cell_mut(index).reason = CloseReason::Stall;
                    *close_wait = close_wait
                        .checked_add(1)
                        .unwrap_or_else(|| invariant("operation"));
                }
                metadata.enqueue(index);
            });
            self.next_sweep = now + SWEEP_PERIOD;
        }
    }
    fn counts(&mut self) {
        let ring = self.rings.take_counts();
        add(&mut self.stats.ring_enters, ring.ring_enters);
        add(&mut self.stats.enter_retries, ring.enter_retries);
        add(&mut self.stats.sq_full, ring.sq_full);
        add(&mut self.stats.cq_overflow, ring.cq_overflow);
        let store = self.store.take_counts();
        add(&mut self.stats.reader_ok, store.reader_ok);
        add(&mut self.stats.reader_not_found, store.reader_not_found);
        add(&mut self.stats.reader_space, store.reader_space);
        add(&mut self.stats.fallback_txn, store.fallback_txn);
        add(&mut self.stats.fallback_cookie, store.fallback_cookie);
        add(&mut self.stats.fallback_cursor, store.fallback_cursor);
        add(&mut self.stats.fallback_record, store.fallback_record);
        add(&mut self.stats.fallback_span, store.fallback_span);
        add(&mut self.stats.fallback_missing, store.fallback_missing);
        add(&mut self.stats.checked, store.checked);
        add(&mut self.stats.commit_fail, store.commit_fail);
        add(&mut self.stats.auto_rollback, store.auto_rollback);
        let wal = self.wal.take_counts(self.now);
        add(&mut self.stats.passes, wal.passes);
        add(&mut self.stats.pass_busy, wal.pass_busy);
        add(&mut self.stats.pass_errors, wal.pass_errors);
        add(&mut self.stats.slices, wal.slices);
        add(&mut self.stats.fsyncs, wal.fsyncs);
        add(&mut self.stats.gate_holds, wal.gate_holds);
        add(&mut self.stats.wal_restarts, wal.wal_restarts);
        add(&mut self.stats.restart_writes, wal.restart_writes);
        self.stats.fsync_us_max = self.stats.fsync_us_max.max(wal.fsync_us_max);
        self.stats.gate_hold_us_max = self.stats.gate_hold_us_max.max(wal.gate_hold_us_max);
        self.stats.wal_frames_max = self.stats.wal_frames_max.max(wal.wal_frames_max);
        self.stats.commit_frames_max = self.stats.commit_frames_max.max(wal.commit_frames_max);
        self.stats.header_sync_us_max = self.stats.header_sync_us_max.max(wal.header_sync_us_max);
        let (hits, misses) = self.store.cache_counts();
        if hits < self.cache.0 || misses < self.cache.1 {
            invariant("cache-count");
        }
        let previous = self.cache;
        add(&mut self.stats.cache_hit, hits - previous.0);
        add(&mut self.stats.cache_miss, misses - previous.1);
        self.cache = (hits, misses);
        let usage = self.conns.usage();
        self.stats.input_bytes_max = self.stats.input_bytes_max.max(usage.input_bytes);
        self.stats.output_bytes_max = self.stats.output_bytes_max.max(usage.output_bytes);
        self.stats.paused_conns_max = self.stats.paused_conns_max.max(self.metadata.output_paused);
        self.stats.rearm_max = self
            .stats
            .rearm_max
            .max(u64::from(usage.rearm.into_iter().max().unwrap_or(0)));
        let peaks = [
            &mut self.stats.live_r0_max,
            &mut self.stats.live_r1_max,
            &mut self.stats.live_r2_max,
            &mut self.stats.live_r3_max,
            &mut self.stats.live_r4_max,
            &mut self.stats.live_r5_max,
            &mut self.stats.live_r6_max,
            &mut self.stats.live_r7_max,
        ];
        for (ring, peak) in (0_u8..).zip(peaks) {
            *peak = (*peak).max(u64::from(self.conns.live(ring)));
        }
        if !self.batch_commit {
            self.stats.w_us_min = Some(
                self.stats
                    .w_us_min
                    .map_or(self.window.us, |value| value.min(self.window.us)),
            );
            self.stats.w_us_max = self.stats.w_us_max.max(self.window.us);
        }
        // Relaxed: this atomic publishes only its own value; no other data is read through it.
        self.stats.stalls = self.progress.stalls.load(Ordering::Relaxed);
    }
    fn trim_scratch(&mut self) {
        for scratch in [&mut self.chunk, &mut self.unescape] {
            scratch.clear();
            if scratch.capacity() > conn::KEEP_MAX as usize {
                *scratch = Vec::with_capacity(conn::KEEP_MAX as usize);
            }
        }
    }
}
