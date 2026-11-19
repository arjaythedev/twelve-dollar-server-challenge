//! Single-owner multi-ring direct-descriptor io_uring server.
//!
//! Up to 8 lazy lanes (`65,471` direct descriptor slots per lane by default),
//! each backed by a mapped `PBUF_RING` buffer ring, `4 × SO_REUSEPORT` 4096-
//! backlog listeners, single-parse HTTP framing (`ParsedOffsets`), drain-first
//! CQE harvesting, and bounded aggregate input/output/memory admission.

use std::collections::VecDeque;
use std::io;
use std::mem;
use std::time::{Duration, Instant};

use crate::app::App;
use crate::buffer::Buffer;
use crate::http::{self, ParseOutcome, ParsedOffsets};
use crate::l2_uring::{Completion, Ring, RingOpaque};
use crate::sys::MemGuard;

const DEFAULT_MAX_LANES: usize = 8;
const DEFAULT_SLOTS_PER_LANE: usize = 65_471;
const DEFAULT_BUFFER_COUNT: u32 = 1024;
const MAX_BUFFER_COUNT: u32 = 32_768;
const ACCEPT_DEPTH: usize = 128;
const RING_ENTRIES: u32 = 2048;
const CQ_ENTRIES: u32 = 8192;
const BUFFER_GROUP: u16 = 1;
const DRAIN_BUDGET: usize = 8_192;

#[cfg(not(l2_test_byte_budget))]
const INPUT_MAX_BYTES: usize = 64 * 1024 * 1024;
#[cfg(l2_test_byte_budget)]
const INPUT_MAX_BYTES: usize = 32 * 1024;
const INPUT_POOL_MAX_BUFFERS: usize = 256;
const INPUT_POOL_MAX_BYTES: usize = 1024 * 1024;
const INPUT_POOL_MAX_BUFFER: usize = 32 * 1024;

#[cfg(not(l2_test_byte_budget))]
const OUTPUT_MAX_BYTES: usize = 64 * 1024 * 1024;
#[cfg(l2_test_byte_budget)]
const OUTPUT_MAX_BYTES: usize = 64 * 1024;
const OUTPUT_POOL_MAX_BUFFERS: usize = 256;
const OUTPUT_POOL_MAX_BYTES: usize = 1024 * 1024;
const OUTPUT_POOL_MAX_BUFFER: usize = 64 * 1024;

const REQUESTS_PER_BATCH: usize = 512;
const ERROR_RESERVE: usize = 512;
const ACCEPT_RETRY: Duration = Duration::from_millis(100);
const MAX_OP_GENERATION: u64 = 1u64 << 45;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum OpKind {
    Accept,
    Receive,
    Send,
    Close,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SlotState {
    Free,
    Accepting,
    Receiving,
    WaitingForBuffer,
    Ready,
    Sending,
    Closing,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ReadyAction {
    Request,
    Continue,
    BadRequest,
    Close,
}

#[derive(Clone, Copy)]
struct Entry {
    lane: u16,
    slot: u16,
    generation: u64,
}

struct Conn {
    state: SlotState,
    generation: u64,
    op_generation: u64,
    operation: Option<OpKind>,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>,
    parsed: Option<ParsedOffsets>,
    sent: usize,
    close_after: bool,
    continue_sent: bool,
    read_eof: bool,
    action: ReadyAction,
    interim_response: bool,
    accept_listener: u8,
}

impl Conn {
    fn new() -> Self {
        Self {
            state: SlotState::Free,
            generation: 0,
            op_generation: 0,
            operation: None,
            inbuf: Vec::new(),
            outbuf: Vec::new(),
            parsed: None,
            sent: 0,
            close_after: false,
            continue_sent: false,
            read_eof: false,
            action: ReadyAction::Close,
            interim_response: false,
            accept_listener: 0,
        }
    }
}

struct Lane {
    ring: Ring,
    conns: Vec<Conn>,
    free_slots: Vec<u16>,
    waiting_buffers: VecDeque<(u16, u64)>,
    accept_inflight: usize,
    staged: usize,
}

impl Lane {
    fn new(slots_per_lane: usize, buffer_count: u32) -> io::Result<Self> {
        let ring = Ring::new(
            RING_ENTRIES,
            CQ_ENTRIES,
            slots_per_lane as u32,
            BUFFER_GROUP,
            buffer_count,
        )?;
        let mut free_slots = Vec::new();
        free_slots
            .try_reserve_exact(slots_per_lane)
            .map_err(|_| io::Error::new(io::ErrorKind::OutOfMemory, "slot table allocation failed"))?;
        free_slots.extend((0..slots_per_lane as u16).rev());

        let mut conns = Vec::new();
        conns
            .try_reserve_exact(slots_per_lane)
            .map_err(|_| io::Error::new(io::ErrorKind::OutOfMemory, "connection table allocation failed"))?;
        conns.resize_with(slots_per_lane, Conn::new);

        Ok(Self {
            ring,
            conns,
            free_slots,
            waiting_buffers: VecDeque::with_capacity(slots_per_lane.min(1024)),
            accept_inflight: 0,
            staged: 0,
        })
    }
}

#[inline]
fn pack_key(op_gen: u64, lane: usize, slot: u16) -> u64 {
    (op_gen << 19) | (((lane as u64) & 0x7) << 16) | (slot as u64)
}

#[inline]
fn unpack_key(key: u64) -> (usize, u16, u64) {
    let slot = (key & 0xffff) as u16;
    let lane = ((key >> 16) & 0x7) as usize;
    let op_gen = key >> 19;
    (lane, slot, op_gen)
}

fn parse_env_usize(name: &str, default: usize, min: usize, max: usize) -> io::Result<usize> {
    match std::env::var(name) {
        Ok(val) => {
            let parsed: usize = val.parse().map_err(|_| {
                io::Error::new(io::ErrorKind::InvalidInput, format!("invalid {name}: {val}"))
            })?;
            if parsed < min || parsed > max {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("{name} must be within {min}..={max}, got {parsed}"),
                ));
            }
            Ok(parsed)
        }
        Err(_) => Ok(default),
    }
}

pub struct Server {
    listeners: Vec<i32>,
    listener_inflight: Vec<usize>,
    lanes: Vec<Lane>,
    max_lanes: usize,
    slots_per_lane: usize,
    buffer_count: u32,
    app: App,
    mem: MemGuard,
    ready: Vec<Entry>,
    deferred_writes: Vec<Entry>,
    batch_reads: Vec<Entry>,
    batch_writes: Vec<Entry>,
    pending_batch: Vec<Entry>,
    input_pool: Vec<Vec<u8>>,
    input_pool_bytes: usize,
    input_bytes: usize,
    output_pool: Vec<Vec<u8>>,
    output_pool_bytes: usize,
    output_bytes: usize,
    accept_retry_at: Option<Instant>,
    write_defer_deadline: Option<Instant>,
    last_wal_counts: (u64, u64, u64),
    diagnostics: bool,
    enobufs_count: u64,
    rearmed_count: u64,
}

impl Server {
    pub fn new(listeners: Vec<i32>, db_path: &str, secret: &str) -> io::Result<Self> {
        if listeners.is_empty() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "at least one listener required"));
        }
        let max_lanes = parse_env_usize("FEED_L2_TEST_MAX_LANES", DEFAULT_MAX_LANES, 1, 8)?;
        let slots_per_lane = parse_env_usize(
            "FEED_L2_TEST_SLOTS_PER_LANE",
            DEFAULT_SLOTS_PER_LANE,
            1,
            DEFAULT_SLOTS_PER_LANE,
        )?;
        let buffer_count = parse_env_usize(
            "FEED_L2_TEST_BUFFER_COUNT",
            DEFAULT_BUFFER_COUNT as usize,
            8,
            MAX_BUFFER_COUNT as usize,
        )? as u32;
        if !buffer_count.is_power_of_two() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "FEED_L2_TEST_BUFFER_COUNT must be a power of 2",
            ));
        }

        let mut lanes = Vec::with_capacity(max_lanes);
        lanes.push(Lane::new(slots_per_lane, buffer_count)?);

        let mut app = App::new_single_owner(db_path, secret);
        app.db.enable_wal_checkpointer(db_path);
        let listener_count = listeners.len();
        let mut server = Self {
            listeners,
            listener_inflight: vec![0; listener_count],
            lanes,
            max_lanes,
            slots_per_lane,
            buffer_count,
            app,
            mem: MemGuard::new(),
            ready: Vec::with_capacity(slots_per_lane.min(2048)),
            deferred_writes: Vec::with_capacity(REQUESTS_PER_BATCH),
            batch_reads: Vec::with_capacity(REQUESTS_PER_BATCH),
            batch_writes: Vec::with_capacity(REQUESTS_PER_BATCH),
            pending_batch: Vec::with_capacity(REQUESTS_PER_BATCH),
            input_pool: Vec::with_capacity(INPUT_POOL_MAX_BUFFERS),
            input_pool_bytes: 0,
            input_bytes: 0,
            output_pool: Vec::with_capacity(OUTPUT_POOL_MAX_BUFFERS),
            output_pool_bytes: 0,
            output_bytes: 0,
            accept_retry_at: None,
            write_defer_deadline: None,
            last_wal_counts: (0, 0, 0),
            diagnostics: std::env::var("FEED_L2_DIAGNOSTICS").as_deref() == Ok("1"),
            enobufs_count: 0,
            rearmed_count: 0,
        };
        if server.diagnostics {
            eprintln!(
                "l2_test startup listeners={} lanes=1 max_lanes={} slots_per_lane={} buffers={}",
                server.listeners.len(),
                server.max_lanes,
                server.slots_per_lane,
                server.buffer_count
            );
        }
        server.fill_accepts()?;
        server.submit_all_staged()?;
        Ok(server)
    }

    fn key_for(&mut self, lane: usize, slot: u16) -> io::Result<u64> {
        let c = &mut self.lanes[lane].conns[slot as usize];
        if c.operation.is_some() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "slot already has an outstanding operation",
            ));
        }
        c.op_generation = c
            .op_generation
            .checked_add(1)
            .filter(|v| *v < MAX_OP_GENERATION)
            .ok_or_else(|| io::Error::new(io::ErrorKind::Other, "operation generation exhausted"))?;
        Ok(pack_key(c.op_generation, lane, slot))
    }

    fn stage_accept(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        let key = self.key_for(lane, slot)?;
        // Balance armed accepts evenly across SO_REUSEPORT listeners so no
        // hash bucket can starve even when connections arrive unevenly.
        let li = self
            .listener_inflight
            .iter()
            .enumerate()
            .min_by_key(|&(_, &c)| c)
            .map(|(i, _)| i)
            .unwrap_or(0);
        let listener = self.listeners[li];
        match unsafe { self.lanes[lane].ring.accept(listener, slot as u32, key) } {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                self.submit_lane_staged(lane)?;
                unsafe { self.lanes[lane].ring.accept(listener, slot as u32, key)?; }
            }
            Err(e) => return Err(e),
        }
        let ln = &mut self.lanes[lane];
        let c = &mut ln.conns[slot as usize];
        c.state = SlotState::Accepting;
        c.operation = Some(OpKind::Accept);
        c.accept_listener = li as u8;
        self.listener_inflight[li] += 1;
        ln.accept_inflight += 1;
        ln.staged += 1;
        self.maybe_submit(lane)
    }

    fn stage_receive(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        let key = self.key_for(lane, slot)?;
        match unsafe { self.lanes[lane].ring.recv_select(slot as u32, key) } {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                self.submit_lane_staged(lane)?;
                unsafe { self.lanes[lane].ring.recv_select(slot as u32, key)?; }
            }
            Err(e) => return Err(e),
        }
        let ln = &mut self.lanes[lane];
        let c = &mut ln.conns[slot as usize];
        c.state = SlotState::Receiving;
        c.operation = Some(OpKind::Receive);
        ln.staged += 1;
        self.maybe_submit(lane)
    }

    fn stage_send(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        let key = self.key_for(lane, slot)?;
        let c = &self.lanes[lane].conns[slot as usize];
        let remaining = c.outbuf.len().saturating_sub(c.sent);
        if remaining == 0 || remaining > u32::MAX as usize {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid L2 send length"));
        }
        let ptr = unsafe { c.outbuf.as_ptr().add(c.sent) };
        match unsafe { self.lanes[lane].ring.send(slot as u32, ptr, remaining as u32, key) } {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                self.submit_lane_staged(lane)?;
                unsafe { self.lanes[lane].ring.send(slot as u32, ptr, remaining as u32, key)?; }
            }
            Err(e) => return Err(e),
        }
        let ln = &mut self.lanes[lane];
        let c = &mut ln.conns[slot as usize];
        c.state = SlotState::Sending;
        c.operation = Some(OpKind::Send);
        ln.staged += 1;
        self.maybe_submit(lane)
    }

    fn stage_close(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        let key = self.key_for(lane, slot)?;
        match unsafe { self.lanes[lane].ring.close(slot as u32, key) } {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                self.submit_lane_staged(lane)?;
                unsafe { self.lanes[lane].ring.close(slot as u32, key)?; }
            }
            Err(e) => return Err(e),
        }
        let ln = &mut self.lanes[lane];
        let c = &mut ln.conns[slot as usize];
        c.state = SlotState::Closing;
        c.operation = Some(OpKind::Close);
        ln.staged += 1;
        self.maybe_submit(lane)
    }

    #[inline]
    fn maybe_submit(&mut self, lane: usize) -> io::Result<()> {
        if self.lanes[lane].staged >= 512 {
            self.submit_lane_staged(lane)?;
        }
        Ok(())
    }

    fn submit_lane_staged(&mut self, lane: usize) -> io::Result<()> {
        let ln = &mut self.lanes[lane];
        while ln.staged != 0 {
            let n = ln.ring.submit()? as usize;
            if n == 0 || n > ln.staged {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "io_uring failed to submit staged operations",
                ));
            }
            ln.staged -= n;
        }
        Ok(())
    }

    fn submit_all_staged(&mut self) -> io::Result<()> {
        for lane in 0..self.lanes.len() {
            if self.lanes[lane].staged != 0 {
                self.submit_lane_staged(lane)?;
            }
        }
        Ok(())
    }

    fn fill_accepts(&mut self) -> io::Result<()> {
        if self.accept_retry_at.map_or(false, |at| Instant::now() < at) {
            return Ok(());
        }
        self.accept_retry_at = None;
        if !self.mem.allows_accepts() {
            self.accept_retry_at = Some(Instant::now() + ACCEPT_RETRY);
            return Ok(());
        }

        let target_accepts = ACCEPT_DEPTH
            .min((self.slots_per_lane / 4).max(4))
            .min(self.slots_per_lane);
        let mut total_accept_inflight: usize =
            self.lanes.iter().map(|l| l.accept_inflight).sum();

        while total_accept_inflight < target_accepts {
            if let Some(lane_idx) = self.lanes.iter().position(|l| !l.free_slots.is_empty()) {
                let slot = self.lanes[lane_idx].free_slots.pop().unwrap();
                let c = &mut self.lanes[lane_idx].conns[slot as usize];
                c.generation = c.generation.checked_add(1).ok_or_else(|| {
                    io::Error::new(io::ErrorKind::Other, "slot generation exhausted")
                })?;
                if let Err(e) = self.stage_accept(lane_idx, slot) {
                    self.lanes[lane_idx].free_slots.push(slot);
                    self.accept_retry_at = Some(Instant::now() + ACCEPT_RETRY);
                    return Err(e);
                }
                total_accept_inflight += 1;
            } else if self.lanes.len() < self.max_lanes {
                if !self.mem.allows_accepts() {
                    self.accept_retry_at = Some(Instant::now() + ACCEPT_RETRY);
                    break;
                }
                let next_idx = self.lanes.len();
                match Lane::new(self.slots_per_lane, self.buffer_count) {
                    Ok(new_lane) => {
                        self.lanes.push(new_lane);
                        if self.diagnostics {
                            eprintln!(
                                "l2_test lane_opened={} total_lanes={} slots_per_lane={} buffers={}",
                                next_idx,
                                self.lanes.len(),
                                self.slots_per_lane,
                                self.buffer_count
                            );
                        }
                    }
                    Err(e) => {
                        if self.diagnostics {
                            eprintln!("l2_test lane_open_failed={} err={e}", next_idx);
                        }
                        self.accept_retry_at = Some(Instant::now() + ACCEPT_RETRY);
                        break;
                    }
                }
            } else {
                break;
            }
        }
        Ok(())
    }

    fn recycle_input(&mut self, mut buf: Vec<u8>) {
        let cap = buf.capacity();
        if cap == 0
            || cap > INPUT_POOL_MAX_BUFFER
            || self.input_pool.len() >= INPUT_POOL_MAX_BUFFERS
            || self.input_pool_bytes > INPUT_POOL_MAX_BYTES.saturating_sub(cap)
        {
            return;
        }
        buf.clear();
        self.input_pool_bytes += cap;
        self.input_pool.push(buf);
    }

    fn take_input(&mut self, lane: usize, slot: u16) {
        if self.lanes[lane].conns[slot as usize].inbuf.capacity() != 0 {
            return;
        }
        if let Some(mut buf) = self.input_pool.pop() {
            self.input_pool_bytes -= buf.capacity();
            buf.clear();
            self.input_bytes += buf.capacity();
            self.lanes[lane].conns[slot as usize].inbuf = buf;
        }
    }

    fn release_slot_accounting(&mut self, lane: usize, slot: u16) {
        let c = &mut self.lanes[lane].conns[slot as usize];
        self.input_bytes = self.input_bytes.saturating_sub(c.inbuf.capacity());
        self.output_bytes = self.output_bytes.saturating_sub(c.outbuf.capacity());
        let input = mem::take(&mut c.inbuf);
        let output = mem::take(&mut c.outbuf);
        let generation = c.generation;
        let op_generation = c.op_generation;
        *c = Conn::new();
        c.generation = generation;
        c.op_generation = op_generation;
        self.recycle_input(input);
        self.recycle_output(output);
    }

    fn recycle_output(&mut self, mut buf: Vec<u8>) {
        let cap = buf.capacity();
        if cap == 0
            || cap > OUTPUT_POOL_MAX_BUFFER
            || self.output_pool.len() >= OUTPUT_POOL_MAX_BUFFERS
            || self.output_pool_bytes > OUTPUT_POOL_MAX_BYTES.saturating_sub(cap)
        {
            return;
        }
        buf.clear();
        self.output_pool_bytes += cap;
        self.output_pool.push(buf);
    }

    fn mark_ready(&mut self, lane: usize, slot: u16, action: ReadyAction) -> io::Result<()> {
        let max_ready = self.lanes.len() * self.slots_per_lane;
        let c = &mut self.lanes[lane].conns[slot as usize];
        if c.state == SlotState::Ready {
            return Ok(());
        }
        if self.ready.len() >= max_ready {
            return Err(io::Error::new(io::ErrorKind::OutOfMemory, "ready queue bound exceeded"));
        }
        c.action = action;
        c.state = SlotState::Ready;
        self.ready.push(Entry {
            lane: lane as u16,
            slot,
            generation: c.generation,
        });
        Ok(())
    }

    fn classify_input(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        let c = &mut self.lanes[lane].conns[slot as usize];
        match http::parse_outcome(&c.inbuf) {
            ParseOutcome::Invalid => {
                c.parsed = None;
                self.mark_ready(lane, slot, ReadyAction::BadRequest)
            }
            ParseOutcome::Complete(off) => {
                c.parsed = Some(off);
                self.mark_ready(lane, slot, ReadyAction::Request)
            }
            ParseOutcome::Incomplete { continue_needed } => {
                c.parsed = None;
                if c.inbuf.is_empty() && c.inbuf.capacity() == 0 && c.continue_sent {
                    self.stage_receive(lane, slot)
                } else if c.read_eof {
                    self.mark_ready(lane, slot, ReadyAction::Close)
                } else if !c.continue_sent && continue_needed {
                    self.mark_ready(lane, slot, ReadyAction::Continue)
                } else {
                    self.stage_receive(lane, slot)
                }
            }
        }
    }

    fn prepare_receive_buffer(&mut self, lane: usize, slot: u16, bytes: &[u8]) -> io::Result<bool> {
        let idx = slot as usize;
        let old_len = self.lanes[lane].conns[idx].inbuf.len();
        let max_input = http::MAX_HEAD.saturating_add(http::MAX_BODY);
        if bytes.len() > max_input.saturating_sub(old_len) {
            return Ok(false);
        }
        let old_cap = self.lanes[lane].conns[idx].inbuf.capacity();
        let pooled_cap = if old_cap == 0 {
            self.input_pool.last().map_or(0, Vec::capacity)
        } else {
            0
        };
        let active_cap = old_cap.saturating_add(pooled_cap);
        let wanted = old_len.saturating_add(bytes.len());
        let projected = self
            .input_bytes
            .saturating_add(pooled_cap)
            .saturating_add(wanted.saturating_sub(active_cap));
        if projected > INPUT_MAX_BYTES {
            if self.diagnostics {
                eprintln!(
                    "l2_test input_budget_rejected projected={} limit={}",
                    projected, INPUT_MAX_BYTES
                );
            }
            return Ok(false);
        }
        self.take_input(lane, slot);
        let old_cap = self.lanes[lane].conns[idx].inbuf.capacity();
        if self.lanes[lane].conns[idx].inbuf.try_reserve_exact(bytes.len()).is_err() {
            return Ok(false);
        }
        let new_cap = self.lanes[lane].conns[idx].inbuf.capacity();
        let delta = new_cap.saturating_sub(old_cap);
        if self.input_bytes.saturating_add(delta) > INPUT_MAX_BYTES {
            if self.diagnostics {
                eprintln!(
                    "l2_test input_allocator_overshoot capacity={} bytes={} limit={}",
                    new_cap, self.input_bytes, INPUT_MAX_BYTES
                );
            }
            self.input_bytes = self.input_bytes.saturating_sub(new_cap);
            self.lanes[lane].conns[idx].inbuf = Vec::new();
            return Ok(false);
        }
        self.input_bytes += delta;
        self.lanes[lane].conns[idx].inbuf.extend_from_slice(bytes);
        Ok(true)
    }

    fn rearm_waiting_buffer(&mut self, lane: usize) -> io::Result<()> {
        while let Some((w_slot, w_gen)) = self.lanes[lane].waiting_buffers.pop_front() {
            let c = &self.lanes[lane].conns[w_slot as usize];
            if c.state == SlotState::WaitingForBuffer && c.generation == w_gen {
                self.stage_receive(lane, w_slot)?;
                self.rearmed_count = self.rearmed_count.saturating_add(1);
                if self.diagnostics {
                    let (kernel, user) = self.lanes[lane].ring.buffer_ownership_counts();
                    eprintln!(
                        "l2_test waiter_rearmed={} lane={} slot={} generation={} kernel={} user={} waiters={}",
                        self.rearmed_count,
                        lane,
                        w_slot,
                        w_gen,
                        kernel,
                        user,
                        self.lanes[lane].waiting_buffers.len()
                    );
                }
                break;
            }
        }
        Ok(())
    }

    fn handle_accept(&mut self, lane: usize, slot: u16, cqe: Completion) -> io::Result<()> {
        self.lanes[lane].accept_inflight = self.lanes[lane].accept_inflight.saturating_sub(1);
        let li = self.lanes[lane].conns[slot as usize].accept_listener as usize;
        if li < self.listener_inflight.len() {
            self.listener_inflight[li] = self.listener_inflight[li].saturating_sub(1);
        }
        self.lanes[lane].conns[slot as usize].operation = None;
        if cqe.result == 0 {
            self.stage_receive(lane, slot)
        } else {
            let c = &mut self.lanes[lane].conns[slot as usize];
            c.state = SlotState::Free;
            self.lanes[lane].free_slots.push(slot);
            self.accept_retry_at = Some(Instant::now() + ACCEPT_RETRY);
            Ok(())
        }
    }

    fn handle_receive(&mut self, lane: usize, slot: u16, cqe: Completion) -> io::Result<()> {
        self.lanes[lane].conns[slot as usize].operation = None;
        if cqe.result == -105 {
            self.enobufs_count = self.enobufs_count.saturating_add(1);
            if self.diagnostics {
                let (kernel, user) = self.lanes[lane].ring.buffer_ownership_counts();
                eprintln!(
                    "l2_test enobufs={} lane={} kernel={} user={} waiters={}",
                    self.enobufs_count,
                    lane,
                    kernel,
                    user,
                    self.lanes[lane].waiting_buffers.len()
                );
            }
            if self.lanes[lane].ring.available_buffer_count() != 0 {
                return self.stage_receive(lane, slot);
            }
            if self.lanes[lane].waiting_buffers.len() >= self.slots_per_lane {
                return Err(io::Error::new(
                    io::ErrorKind::OutOfMemory,
                    "buffer wait queue bound exceeded",
                ));
            }
            let c = &mut self.lanes[lane].conns[slot as usize];
            c.state = SlotState::WaitingForBuffer;
            let gen = c.generation;
            self.lanes[lane].waiting_buffers.push_back((slot, gen));
            return Ok(());
        }

        if let Some(id) = cqe.buffer_id {
            let n = usize::try_from(cqe.result.max(0)).unwrap_or(0);
            let (ptr, cap) = self.lanes[lane].ring.buffer_ptr_and_len(id)?;
            if n > cap {
                let _ = self.lanes[lane].ring.return_buffer(id);
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "selected receive exceeds provided buffer",
                ));
            }
            let admitted = if n != 0 {
                let slice = unsafe { std::slice::from_raw_parts(ptr, n) };
                self.prepare_receive_buffer(lane, slot, slice)?
            } else {
                true
            };
            self.lanes[lane].ring.return_buffer(id)?;
            self.rearm_waiting_buffer(lane)?;
            if n != 0 && !admitted {
                return self.mark_ready(lane, slot, ReadyAction::BadRequest);
            }
        } else if cqe.result > 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "positive receive completion has no selected buffer",
            ));
        }

        if cqe.result == 0 {
            self.lanes[lane].conns[slot as usize].read_eof = true;
        } else if cqe.result < 0 {
            return self.stage_close(lane, slot);
        }
        self.classify_input(lane, slot)
    }

    fn handle_send(&mut self, lane: usize, slot: u16, cqe: Completion) -> io::Result<()> {
        self.lanes[lane].conns[slot as usize].operation = None;
        if cqe.result <= 0 {
            return self.stage_close(lane, slot);
        }
        let idx = slot as usize;
        self.lanes[lane].conns[idx].sent =
            self.lanes[lane].conns[idx].sent.saturating_add(cqe.result as usize);
        if self.lanes[lane].conns[idx].sent < self.lanes[lane].conns[idx].outbuf.len() {
            return self.stage_send(lane, slot);
        }

        let output = mem::take(&mut self.lanes[lane].conns[idx].outbuf);
        self.output_bytes = self.output_bytes.saturating_sub(output.capacity());
        self.recycle_output(output);
        let c = &mut self.lanes[lane].conns[idx];
        c.sent = 0;
        c.interim_response = false;
        if c.close_after {
            return self.stage_close(lane, slot);
        }
        self.classify_input(lane, slot)
    }

    fn handle_completion(&mut self, cqe: Completion) -> io::Result<()> {
        let (lane, slot, sequence) = unpack_key(cqe.user_data);
        let ln = self
            .lanes
            .get_mut(lane)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "completion has invalid lane"))?;
        let c = ln
            .conns
            .get_mut(slot as usize)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "completion has invalid slot"))?;
        if c.op_generation != sequence {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "stale or duplicate operation completion",
            ));
        }
        let op = c
            .operation
            .take()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "completion has no operation record"))?;
        if self.diagnostics {
            eprintln!(
                "l2_test completion op={op:?} lane={lane} slot={slot} result={} buffer={:?}",
                cqe.result, cqe.buffer_id
            );
        }
        match op {
            OpKind::Accept => self.handle_accept(lane, slot, cqe),
            OpKind::Receive => self.handle_receive(lane, slot, cqe),
            OpKind::Send => self.handle_send(lane, slot, cqe),
            OpKind::Close => {
                if cqe.result != 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::Other,
                        format!("direct close failed: {}", cqe.result),
                    ));
                }
                self.release_slot_accounting(lane, slot);
                self.lanes[lane].free_slots.push(slot);
                Ok(())
            }
        }
    }

    fn close_without_reply(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        if self.lanes[lane].conns[slot as usize].operation.is_some() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "cannot close slot with an active operation",
            ));
        }
        self.stage_close(lane, slot)
    }

    fn serve_one(&mut self, lane: usize, slot: u16) -> io::Result<()> {
        let idx = slot as usize;
        let action = self.lanes[lane].conns[idx].action;
        if action == ReadyAction::Close {
            return self.close_without_reply(lane, slot);
        }

        let used_pool = self.output_pool.pop();
        let reusable = if let Some(buf) = used_pool {
            self.output_pool_bytes = self.output_pool_bytes.saturating_sub(buf.capacity());
            buf
        } else {
            Vec::new()
        };
        let available = OUTPUT_MAX_BYTES.saturating_sub(
            self.output_bytes
                .saturating_add(self.output_pool_bytes)
                .saturating_add(self.app.scratch_capacity()),
        );
        let quota = available / 2;
        if quota < ERROR_RESERVE {
            if self.diagnostics {
                eprintln!(
                    "l2_test output_budget_rejected bytes={} pool={} scratch={} limit={}",
                    self.output_bytes,
                    self.output_pool_bytes,
                    self.app.scratch_capacity(),
                    OUTPUT_MAX_BYTES
                );
            }
            return self.close_without_reply(lane, slot);
        }
        let mut out = Buffer::from_vec(reusable, quota);
        out.clear();
        out.reserve(ERROR_RESERVE);
        if out.failed() {
            return self.close_without_reply(lane, slot);
        }
        self.app.output_budget(quota);

        let mut input = mem::take(&mut self.lanes[lane].conns[idx].inbuf);
        let parsed = mem::take(&mut self.lanes[lane].conns[idx].parsed);
        let mut close = false;
        let mut interim = false;
        match action {
            ReadyAction::Continue => {
                out.extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                interim = true;
                self.lanes[lane].conns[idx].continue_sent = true;
            }
            ReadyAction::BadRequest => {
                http::respond(&mut out, 400, crate::app::R_INTERNAL);
                input.clear();
                close = true;
            }
            ReadyAction::Request => match parsed {
                Some(offsets) if (offsets.used as usize) <= input.len() => {
                    let request = offsets.to_request(&input);
                    let used = offsets.used as usize;
                    self.lanes[lane].conns[idx].continue_sent = false;
                    close = self.app.handle(&request, &mut out);
                    input.drain(..used);
                    if close {
                        input.clear();
                    }
                }
                _ => {
                    http::respond(&mut out, 400, crate::app::R_INTERNAL);
                    input.clear();
                    close = true;
                }
            },
            ReadyAction::Close => unreachable!(),
        }

        if out.failed() || self.app.output_failed() {
            if self.diagnostics {
                eprintln!("l2_test output_builder_rejected quota={quota}");
            }
            out.clear();
            http::respond(&mut out, 500, crate::app::R_INTERNAL);
            input.clear();
            close = true;
            interim = false;
        }
        self.app.release_scratch();
        let output = out.into_vec();
        if output.is_empty() {
            self.recycle_output(output);
        } else {
            if self
                .output_bytes
                .saturating_add(self.output_pool_bytes)
                .saturating_add(output.capacity())
                > OUTPUT_MAX_BYTES
            {
                if self.diagnostics {
                    eprintln!(
                        "l2_test output_capacity_rejected output_cap={} bytes={} pool={} limit={}",
                        output.capacity(),
                        self.output_bytes,
                        self.output_pool_bytes,
                        OUTPUT_MAX_BYTES
                    );
                }
                self.input_bytes = self.input_bytes.saturating_sub(input.capacity());
                self.lanes[lane].conns[idx].inbuf = Vec::new();
                return self.close_without_reply(lane, slot);
            }
            self.output_bytes += output.capacity();
            let c = &mut self.lanes[lane].conns[idx];
            c.outbuf = output;
            c.sent = 0;
            c.close_after = close;
            c.interim_response = interim;
        }

        if input.is_empty() {
            self.input_bytes = self.input_bytes.saturating_sub(input.capacity());
            self.recycle_input(input);
            self.lanes[lane].conns[idx].inbuf = Vec::new();
        } else {
            self.lanes[lane].conns[idx].inbuf = input;
        }
        if interim && self.lanes[lane].conns[idx].outbuf.is_empty() {
            self.classify_input(lane, slot)
        } else {
            self.pending_batch.push(Entry {
                lane: lane as u16,
                slot,
                generation: self.lanes[lane].conns[idx].generation,
            });
            Ok(())
        }
    }

    #[inline]
    fn defer_active(&mut self) -> bool {
        if !self.app.db.wal_restart_requested() {
            self.write_defer_deadline = None;
            return false;
        }
        let now = Instant::now();
        let deadline = *self
            .write_defer_deadline
            .get_or_insert_with(|| now + Duration::from_millis(150));
        now < deadline
    }

    fn commit_and_send(&mut self) -> io::Result<()> {
        while !self.ready.is_empty() || (!self.deferred_writes.is_empty() && !self.defer_active()) {
            self.pending_batch.clear();
            self.batch_reads.clear();
            self.batch_writes.clear();
            let deferring = self.defer_active();

            if !deferring && !self.deferred_writes.is_empty() {
                let take = self.deferred_writes.len().min(REQUESTS_PER_BATCH);
                self.batch_writes.extend(self.deferred_writes.drain(..take));
            }
            let room = REQUESTS_PER_BATCH.saturating_sub(self.batch_writes.len());
            if room > 0 && !self.ready.is_empty() {
                let count = self.ready.len().min(room);
                for entry in self.ready.drain(..count) {
                    let c = &self.lanes[entry.lane as usize].conns[entry.slot as usize];
                    if c.state != SlotState::Ready || c.generation != entry.generation {
                        continue;
                    }
                    if c.action == ReadyAction::Request && c.parsed.map_or(false, |p| p.is_post) {
                        if deferring {
                            self.deferred_writes.push(entry);
                        } else {
                            self.batch_writes.push(entry);
                        }
                    } else {
                        self.batch_reads.push(entry);
                    }
                }
            }
            if self.batch_reads.is_empty() && self.batch_writes.is_empty() {
                continue;
            }
            let write = !self.batch_writes.is_empty();
            self.app.begin_batch(write);
            for i in 0..self.batch_reads.len() {
                let entry = self.batch_reads[i];
                let lane = entry.lane as usize;
                let c = &self.lanes[lane].conns[entry.slot as usize];
                if c.state != SlotState::Ready || c.generation != entry.generation {
                    continue;
                }
                self.serve_one(lane, entry.slot)?;
            }
            for i in 0..self.batch_writes.len() {
                let entry = self.batch_writes[i];
                let lane = entry.lane as usize;
                let c = &self.lanes[lane].conns[entry.slot as usize];
                if c.state != SlotState::Ready || c.generation != entry.generation {
                    continue;
                }
                self.serve_one(lane, entry.slot)?;
            }
            let committed = self.app.end_batch();
            let mut to_send = mem::take(&mut self.pending_batch);
            for entry in to_send.drain(..) {
                let lane = entry.lane as usize;
                let idx = entry.slot as usize;
                if self.lanes[lane].conns[idx].generation != entry.generation
                    || self.lanes[lane].conns[idx].state != SlotState::Ready
                {
                    continue;
                }
                if !committed {
                    let old = mem::take(&mut self.lanes[lane].conns[idx].outbuf);
                    let cap = old.capacity();
                    self.output_bytes = self.output_bytes.saturating_sub(cap);
                    let mut error = Buffer::from_vec(old, cap);
                    error.clear();
                    http::respond(&mut error, 500, crate::app::R_INTERNAL);
                    if error.failed() {
                        self.stage_close(lane, entry.slot)?;
                        continue;
                    }
                    self.output_bytes += error.capacity();
                    let c = &mut self.lanes[lane].conns[idx];
                    c.outbuf = error.into_vec();
                    c.close_after = true;
                    c.interim_response = false;
                }
                self.stage_send(lane, entry.slot)?;
            }
            self.pending_batch = to_send;
        }
        if self.diagnostics {
            let (pages, p_ok, r_ok, r_busy) = self.app.db.wal_checkpoint_stats();
            let key = (p_ok, r_ok, r_busy);
            if key != self.last_wal_counts {
                self.last_wal_counts = key;
                eprintln!(
                    "l2_test wal_ckpt wal_pages={pages} passive_ok={p_ok} restart_ok={r_ok} restart_busy={r_busy}"
                );
            }
        }
        Ok(())
    }

    fn check_accounting(&self) {
        let actual_in: usize = self
            .lanes
            .iter()
            .flat_map(|l| l.conns.iter())
            .map(|c| c.inbuf.capacity())
            .sum();
        let actual_out: usize = self
            .lanes
            .iter()
            .flat_map(|l| l.conns.iter())
            .map(|c| c.outbuf.capacity())
            .sum();
        debug_assert_eq!(self.input_bytes, actual_in);
        debug_assert_eq!(self.output_bytes, actual_out);
        debug_assert_eq!(
            self.input_pool_bytes,
            self.input_pool.iter().map(|b| b.capacity()).sum::<usize>()
        );
        debug_assert_eq!(
            self.output_pool_bytes,
            self.output_pool.iter().map(|b| b.capacity()).sum::<usize>()
        );
        debug_assert!(self.input_bytes <= INPUT_MAX_BYTES);
        debug_assert!(self.input_pool_bytes <= INPUT_POOL_MAX_BYTES);
        debug_assert!(self.output_bytes.saturating_add(self.output_pool_bytes) <= OUTPUT_MAX_BYTES);
        debug_assert!(self.ready.len() + self.deferred_writes.len() <= self.lanes.len() * self.slots_per_lane);
        for l in &self.lanes {
            debug_assert!(l.waiting_buffers.len() <= self.slots_per_lane);
        }
    }

    pub fn run(mut self) -> ! {
        let mut wait_handles: Vec<*mut RingOpaque> = Vec::with_capacity(self.max_lanes);
        loop {
            let mut drained = 0usize;
            let mut had_more_cqes = false;
            loop {
                let mut progressed = false;
                for lane_idx in 0..self.lanes.len() {
                    match self.lanes[lane_idx].ring.peek() {
                        Ok(Some(cqe)) => {
                            progressed = true;
                            drained += 1;
                            self.handle_completion(cqe)
                                .unwrap_or_else(|e| panic!("L2 completion: {e}"));
                            if drained >= DRAIN_BUDGET {
                                had_more_cqes = true;
                                break;
                            }
                        }
                        Ok(None) => {}
                        Err(e) => panic!("L2 completion queue: {e}"),
                    }
                }
                if had_more_cqes || !progressed {
                    break;
                }
            }

            self.fill_accepts()
                .unwrap_or_else(|e| panic!("L2 accept staging: {e}"));
            self.submit_all_staged()
                .unwrap_or_else(|e| panic!("L2 submit: {e}"));
            self.commit_and_send()
                .unwrap_or_else(|e| panic!("L2 App batch: {e}"));
            self.submit_all_staged()
                .unwrap_or_else(|e| panic!("L2 send submit: {e}"));
            self.check_accounting();

            let flush_deferred = !self.deferred_writes.is_empty() && !self.defer_active();
            let cq_pending = had_more_cqes
                || !self.ready.is_empty()
                || flush_deferred
                || self.lanes.iter_mut().any(|l| l.ring.cq_ready() > 0);
            if cq_pending {
                continue;
            }

            let mut timeout = self
                .accept_retry_at
                .map(|at| {
                    let left = at.saturating_duration_since(Instant::now());
                    left.as_millis().min(i32::MAX as u128) as i32
                })
                .unwrap_or(-1);
            if !self.deferred_writes.is_empty() {
                timeout = if timeout < 0 { 2 } else { timeout.min(2) };
            } else if self.diagnostics && timeout < 0 {
                timeout = 1000;
            }

            wait_handles.clear();
            for l in &self.lanes {
                wait_handles.push(l.ring.raw_handle());
            }
            let mask = Ring::wait_any_raw(&wait_handles, timeout)
                .unwrap_or_else(|e| panic!("L2 event wait: {e}"));
            if self.diagnostics {
                let staged: usize = self.lanes.iter().map(|l| l.staged).sum();
                let waiters: usize = self.lanes.iter().map(|l| l.waiting_buffers.len()).sum();
                let buffers: usize = self.lanes.iter().map(|l| l.ring.available_buffer_count()).sum();
                eprintln!(
                    "l2_test event_wait mask={mask} timeout_ms={timeout} lanes={} staged={staged} ready={} waiters={waiters} buffers={buffers}",
                    self.lanes.len(),
                    self.ready.len(),
                );
            }
            if mask == 0 && self.accept_retry_at.map_or(false, |at| Instant::now() >= at) {
                self.accept_retry_at = None;
            }
        }
    }
}

#[cfg(test)]
mod key_tests {
    use super::{pack_key, unpack_key, MAX_OP_GENERATION};

    #[test]
    fn key_packs_and_unpacks_all_lanes_and_slots() {
        for lane in 0..8usize {
            for slot in [0u16, 1, 63, 1024, 65_470] {
                for gen in [1u64, 42, MAX_OP_GENERATION - 1] {
                    let k = pack_key(gen, lane, slot);
                    assert_eq!(unpack_key(k), (lane, slot, gen));
                }
            }
        }
    }
}
