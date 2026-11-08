use std::alloc::{Layout, alloc_zeroed, dealloc, handle_alloc_error};
use std::ptr::NonNull;

pub(crate) const INPUT_BUDGET: u64 = if cfg!(twelve_small_limits) {
    262_144
} else {
    16_777_216
};
pub(crate) const OUTPUT_BUDGET: u64 = if cfg!(twelve_small_limits) {
    131_185
} else {
    33_554_432
};
pub(crate) const INPUT_MAX: u32 = 131_072;
pub(crate) const UNSENT_MAX: u32 = 65_536;
pub(crate) const KEEP_MAX: u32 = 16_384;
pub(crate) const KEEP_COUNT: u32 = 1_024;
pub(crate) const STALL_SECS: u32 = if cfg!(twelve_small_limits) { 2 } else { 120 };
const NONE: u32 = u32::MAX;

pub(crate) mod flag {
    pub(crate) const LIVE: u32 = 1;
    pub(crate) const RECEIVING: u32 = 1 << 1;
    pub(crate) const ONCE: u32 = 1 << 2;
    pub(crate) const POLLING: u32 = 1 << 3;
    pub(crate) const SENDING: u32 = 1 << 4;
    pub(crate) const CLOSING: u32 = 1 << 5;
    pub(crate) const CLOSED: u32 = 1 << 6;
    pub(crate) const READ_EOF: u32 = 1 << 7;
    pub(crate) const PAUSED: u32 = 1 << 8;
    pub(crate) const IN_SERVICE: u32 = 1 << 9;
    pub(crate) const CONTINUE_SENT: u32 = 1 << 10;
    pub(crate) const CLOSE_AFTER: u32 = 1 << 11;
    pub(crate) const READY: u32 = 1 << 12;
    pub(crate) const REARM: u32 = 1 << 13;
    pub(crate) const WAITING: u32 = 1 << 14;
    pub(crate) const HELD: u32 = 1 << 15;
    pub(crate) const SWEEP: u32 = 1 << 16;
    // A close whose state lives in this record alone (LEAN), its place on the lean close list
    // (LEAN_LIST) and its SIOCOUTQ state: asked and waiting (OUTQ_WAIT), asked (OUTQ_DONE),
    // answered zero unacknowledged bytes (OUTQ_ZERO).
    pub(crate) const LEAN: u32 = 1 << 17;
    pub(crate) const LEAN_LIST: u32 = 1 << 18;
    pub(crate) const OUTQ_WAIT: u32 = 1 << 19;
    pub(crate) const OUTQ_DONE: u32 = 1 << 20;
    pub(crate) const OUTQ_ZERO: u32 = 1 << 21;
    // The safe build's drain: the write side is shut down, receives keep running and their bytes
    // are discarded.
    pub(crate) const DRAINING: u32 = 1 << 22;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub(crate) struct ConnId {
    pub(crate) ring: u8,
    pub(crate) slot: u16,
    pub(crate) generation: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Op {
    Recv,
    Once,
    Poll,
    Send,
    Close,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum End {
    Stale,
    Live,
    Freed,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Pause {
    Output,
    Input { want: u32, bid: u16, len: u16 },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Reply {
    Continue,
    Final { close: bool },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct Conn {
    pub(crate) flags: u32,
    pub(crate) input_len: u32,
    pub(crate) input_room: u32,
    pub(crate) unsent: u32,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct Usage {
    pub(crate) input_bytes: u64,
    pub(crate) output_bytes: u64,
    pub(crate) paused: u32,
    pub(crate) waiting: u32,
    pub(crate) ready: u32,
    pub(crate) rearm: [u32; 8],
    pub(crate) holding: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Unacked {
    Zero,
    Bytes,
}

#[repr(C, align(64))]
#[derive(Default)]
struct Record {
    generation: u32,
    flags: u32,
    touched: u32,
    input: u32,
    output: u32,
    flight: u32,
    want: u32,
    held_bid: u16,
    held_len: u16,
    ready_prev: u32,
    ready_next: u32,
    wait_prev: u32,
    wait_next: u32,
    rearm_prev: u16,
    rearm_next: u16,
    sweep_prev: u16,
    sweep_next: u16,
}
const _: () = assert!(size_of::<Record>() == 64);

#[derive(Clone, Copy)]
struct List {
    head: u32,
    tail: u32,
    len: u32,
}
impl List {
    fn new() -> Self {
        Self {
            head: NONE,
            tail: NONE,
            len: 0,
        }
    }
}
#[derive(Clone, Copy)]
enum Kind {
    Ready,
    Wait,
    Rearm,
    Sweep,
    // The lean close list reuses the ready links, free once close_started unlinked Ready.
    Lean,
}
impl Kind {
    fn flag(self) -> u32 {
        match self {
            Self::Ready => flag::READY,
            Self::Wait => flag::WAITING,
            Self::Rearm => flag::REARM,
            Self::Sweep => flag::SWEEP,
            Self::Lean => flag::LEAN_LIST,
        }
    }
}

struct Table {
    block: NonNull<u8>,
    layout: Layout,
    records: NonNull<Record>,
    len: usize,
    live: u32,
    rearm: List,
    sweep: List,
}
impl Table {
    // Measured basis of this layout: on Rust 1.94.0 Box::new_zeroed_slice writes
    // zeros over the align(64) layout at startup, while alloc_zeroed at align 16 reaches calloc,
    // which leaves the kernel's fresh zeroed pages untouched; the native record-table test bounds the
    // RssAnon rise after Conns::new(65,535) below 4,096 kB, against about 88,135 kB if written.
    fn new(len: usize) -> Self {
        let size = len
            .checked_mul(64)
            .and_then(|n| n.checked_add(48))
            .expect("record table layout");
        let layout = Layout::from_size_align(size, 16).expect("record table layout");
        // SAFETY: layout has nonzero size and valid alignment. Align 16 keeps
        // System's zeroed allocation on calloc without touching fresh pages.
        let block = NonNull::new(unsafe { alloc_zeroed(layout) })
            .unwrap_or_else(|| handle_alloc_error(layout));
        let offset = (64 - (block.as_ptr().addr() & 63)) & 63;
        // SAFETY: NonNull::add needs the offset inside the allocation: calloc's 16-byte alignment
        // makes offset at most 48, inside a block of len * 64 + 48 bytes.
        let start = unsafe { block.add(offset) };
        // The aligned range of len records fits in the block, and all-zero bytes are a free Record.
        let records = start.cast::<Record>();
        Self {
            block,
            layout,
            records,
            len,
            live: 0,
            rearm: List::new(),
            sweep: List::new(),
        }
    }
    fn records(&self) -> &[Record] {
        // SAFETY: Table owns this aligned, initialized range until Drop; the
        // shared borrow excludes every mutable record view for its lifetime.
        unsafe { std::slice::from_raw_parts(self.records.as_ptr(), self.len) }
    }
    fn records_mut(&mut self) -> &mut [Record] {
        // SAFETY: exclusive Table ownership gives the only mutable view. The
        // aligned initialized range is contained in Table's live allocation.
        unsafe { std::slice::from_raw_parts_mut(self.records.as_ptr(), self.len) }
    }
}
impl Drop for Table {
    fn drop(&mut self) {
        // SAFETY: Record holds no resources; block and layout are exactly those
        // allocated in new, and no record view can outlive this exclusive Drop.
        unsafe {
            dealloc(self.block.as_ptr(), self.layout);
        }
    }
}

struct Storage {
    bytes: Vec<u8>,
    start: u32,
    charge: u32,
}
const _: () = assert!(size_of::<Storage>() == 32);
#[derive(Clone, Copy)]
enum BufferKind {
    Input,
    Output,
}

pub(crate) struct Conns {
    tables: [Table; 8],
    arena: Vec<Storage>,
    kept_in: Vec<u32>,
    kept_out: Vec<u32>,
    empty: Vec<u32>,
    now_s: u32,
    in_charged: u64,
    in_pooled: u64,
    out_charged: u64,
    out_pooled: u64,
    paused: u32,
    ready: List,
    wait: List,
    lean: List,
}

impl Conns {
    pub(crate) fn new(slots_per_ring: u32) -> Self {
        let slots = usize::try_from(slots_per_ring).expect("slot count fits usize");
        let arena_len = slots
            .checked_mul(24)
            .and_then(|n| n.checked_add(2 * KEEP_COUNT as usize))
            .expect("storage arena layout");
        Self {
            tables: std::array::from_fn(|_| Table::new(slots)),
            arena: Vec::with_capacity(arena_len),
            kept_in: Vec::with_capacity(KEEP_COUNT as usize),
            kept_out: Vec::with_capacity(KEEP_COUNT as usize),
            empty: Vec::with_capacity(arena_len),
            now_s: 0,
            in_charged: 0,
            in_pooled: 0,
            out_charged: 0,
            out_pooled: 0,
            paused: 0,
            ready: List::new(),
            wait: List::new(),
            lean: List::new(),
        }
    }
    pub(crate) fn tick(&mut self, now_s: u32) {
        self.now_s = now_s;
    }
    fn key(&self, ring: u8, slot: u16) -> u32 {
        assert!(usize::from(ring) < 8, "ring index out of range");
        let table = self
            .tables
            .get(usize::from(ring))
            .expect("ring < 8, asserted above");
        assert!(usize::from(slot) < table.len, "slot out of range");
        (u32::from(ring) << 16) | u32::from(slot)
    }
    fn record(&self, key: u32) -> &Record {
        self.tables
            .get((key >> 16) as usize)
            .expect("ring < 8, asserted by key")
            .records()
            .get((key & 65535) as usize)
            .expect("slot < slots_per_ring, asserted by key")
    }
    fn record_mut(&mut self, key: u32) -> &mut Record {
        self.tables
            .get_mut((key >> 16) as usize)
            .expect("ring < 8, asserted by key")
            .records_mut()
            .get_mut((key & 65535) as usize)
            .expect("slot < slots_per_ring, asserted by key")
    }
    fn checked(&self, id: ConnId, closing: bool) -> u32 {
        let key = self.key(id.ring, id.slot);
        let record = self.record(key);
        assert!(
            record.flags & flag::LIVE != 0 && record.generation == id.generation,
            "stale connection id"
        );
        assert!(
            closing || record.flags & flag::CLOSING == 0,
            "connection closing"
        );
        key
    }
    fn serving(&self, id: ConnId) -> u32 {
        let key = self.checked(id, false);
        assert!(
            self.record(key).flags & flag::DRAINING == 0,
            "connection draining"
        );
        key
    }
    fn id(&self, key: u32) -> ConnId {
        ConnId {
            ring: u8::try_from(key >> 16).expect("ring key"),
            slot: u16::try_from(key & 65535).expect("slot key"),
            generation: self.record(key).generation,
        }
    }
    /// # Panics
    /// Panics for a ring or slot outside the allocated tables.
    pub(crate) fn open(&mut self, ring: u8, slot: u16) -> Option<ConnId> {
        let key = self.key(ring, slot);
        if self.record(key).flags & flag::LIVE != 0 {
            return None;
        }
        let generation = self.record(key).generation;
        *self.record_mut(key) = Record {
            generation,
            flags: flag::LIVE,
            touched: self.now_s,
            ready_prev: NONE,
            ready_next: NONE,
            wait_prev: NONE,
            wait_next: NONE,
            rearm_prev: u16::MAX,
            rearm_next: u16::MAX,
            sweep_prev: u16::MAX,
            sweep_next: u16::MAX,
            ..Record::default()
        };
        let table = self
            .tables
            .get_mut(usize::from(ring))
            .expect("ring < 8, asserted by key");
        // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
        table.live += 1;
        Some(self.id(key))
    }
    fn storage(&self, index: u32) -> &Storage {
        // UNSAFE PATH (speed): unchecked arithmetic; a record names a storage by index + 1 and 0 means none, so a named index is at least 1
        let at = (index - 1) as usize;
        self.arena
            .get(at)
            .expect("index - 1 < arena length: a record names only arena entries")
    }
    fn storage_mut(&mut self, index: u32) -> &mut Storage {
        // UNSAFE PATH (speed): unchecked arithmetic; a record names a storage by index + 1 and 0 means none, so a named index is at least 1
        let at = (index - 1) as usize;
        self.arena
            .get_mut(at)
            .expect("index - 1 < arena length: a record names only arena entries")
    }
    fn held_len(&self, index: u32) -> u32 {
        if index == 0 {
            0
        } else {
            let s = self.storage(index);
            // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
            u32::try_from(s.bytes.len()).expect("bounded storage") - s.start
        }
    }
    /// # Panics
    /// Panics for an out-of-range ring or slot; stale generations return None.
    pub(crate) fn get(&self, id: ConnId) -> Option<Conn> {
        let key = self.key(id.ring, id.slot);
        let r = self.record(key);
        if r.flags & flag::LIVE == 0 || r.generation != id.generation {
            return None;
        }
        let input_len = self.held_len(r.input);
        let input_room = if r.input == 0 {
            0
        } else {
            // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
            self.storage(r.input).charge - input_len
        };
        Some(Conn {
            flags: r.flags,
            input_len,
            input_room,
            // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
            unsent: self.held_len(r.output) + self.held_len(r.flight),
        })
    }
    /// # Panics
    /// Panics for a ring outside the eight allocated tables.
    pub(crate) fn live(&self, ring: u8) -> u32 {
        assert!(usize::from(ring) < 8, "ring index out of range");
        self.tables
            .get(usize::from(ring))
            .expect("ring < 8, asserted above")
            .live
    }
    pub(crate) fn usage(&self) -> Usage {
        Usage {
            input_bytes: self.in_charged,
            output_bytes: self.out_charged,
            paused: self.paused,
            waiting: self.wait.len,
            ready: self.ready.len,
            rearm: self.tables.each_ref().map(|table| table.rearm.len),
            holding: self.tables.iter().map(|table| table.sweep.len).sum(),
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing id, an already armed operation, or an
    /// operation other than Recv or Poll.
    pub(crate) fn armed(&mut self, id: ConnId, op: Op) {
        let key = self.checked(id, false);
        assert!(
            matches!(op, Op::Recv | Op::Poll),
            "armed takes Recv or Poll"
        );
        let bit = if op == Op::Recv {
            flag::RECEIVING
        } else {
            flag::POLLING
        };
        assert!(self.record(key).flags & bit == 0, "operation already armed");
        self.record_mut(key).flags |= bit;
    }
    /// # Panics
    /// Panics for out-of-range indices, an operation not armed on a live matching
    /// id, or a completion count beyond the lent extent. Stale ids return Stale.
    pub(crate) fn ended(&mut self, id: ConnId, op: Op, result: i32) -> End {
        let key = self.key(id.ring, id.slot);
        if self.record(key).flags & flag::LIVE == 0 || self.record(key).generation != id.generation
        {
            return End::Stale;
        }
        let bit = match op {
            Op::Recv => flag::RECEIVING,
            Op::Once => flag::ONCE,
            Op::Poll => flag::POLLING,
            Op::Send => flag::SENDING,
            Op::Close => flag::CLOSING,
        };
        assert!(
            self.record(key).flags & bit != 0
                && (op != Op::Close || self.record(key).flags & flag::CLOSED == 0),
            "operation not armed"
        );
        if op == Op::Close {
            self.record_mut(key).flags |= flag::CLOSED;
        } else {
            self.record_mut(key).flags &= !bit;
        }
        if result > 0 && op == Op::Once {
            let input = self.record(key).input;
            let s = self.storage_mut(input);
            let len = s.bytes.len() + usize::try_from(result).expect("positive receive");
            assert!(len <= s.bytes.capacity(), "input range out of range");
            // SAFETY: start_once lent precisely this spare capacity. The terminal
            // completion reports result initialized bytes written by the kernel.
            unsafe {
                s.bytes.set_len(len);
            }
            self.record_mut(key).touched = self.now_s;
        } else if result > 0 && op == Op::Send {
            let flight = self.record(key).flight;
            let s = self.storage_mut(flight);
            let count = u32::try_from(result).expect("positive send");
            // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
            let end = s.start + count;
            assert!(
                usize::try_from(end).expect("bounded send") <= s.bytes.len(),
                "output range out of range"
            );
            s.start = end;
            if self.held_len(flight) == 0 {
                self.record_mut(key).flight = 0;
                self.release(flight, BufferKind::Output);
                self.update_sweep(key);
            }
            self.record_mut(key).touched = self.now_s;
        }
        if op == Op::Once && self.record(key).flags & flag::DRAINING != 0 {
            // The drain discards received bytes, so the one-shot receive's storage goes at once.
            let input = std::mem::take(&mut self.record_mut(key).input);
            if input != 0 {
                self.release(input, BufferKind::Input);
            }
            self.update_sweep(key);
        }
        let flags = self.record(key).flags;
        if flags & (flag::CLOSING | flag::CLOSED) == flag::CLOSING | flag::CLOSED
            && flags & (flag::RECEIVING | flag::ONCE | flag::POLLING | flag::SENDING) == 0
        {
            for kind in [
                Kind::Ready,
                Kind::Wait,
                Kind::Rearm,
                Kind::Sweep,
                Kind::Lean,
            ] {
                self.unlink(key, kind);
            }
            let r = self.record_mut(key);
            let (input, output, flight, generation) =
                (r.input, r.output, r.flight, r.generation.wrapping_add(1));
            *r = Record {
                generation,
                ..Record::default()
            };
            if input != 0 {
                self.release(input, BufferKind::Input);
            }
            if output != 0 {
                self.release(output, BufferKind::Output);
            }
            if flight != 0 {
                self.release(flight, BufferKind::Output);
            }
            let table = self
                .tables
                .get_mut(usize::from(id.ring))
                .expect("id.ring < 8, checked by key");
            // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
            table.live -= 1;
            End::Freed
        } else {
            End::Live
        }
    }
    /// # Panics
    /// Panics for an invalid/stale id or an already closing connection.
    pub(crate) fn close_started(&mut self, id: ConnId) -> Option<u16> {
        let key = self.checked(id, false);
        let held = self.stop_service(key);
        let r = self.record_mut(key);
        r.flags = (r.flags | flag::CLOSING) & !flag::DRAINING;
        self.update_sweep(key);
        held
    }
    /// Starts the safe build's drain: DRAINING set, touched now and the record kept on its sweep
    /// list, after the steps close_started shares (lists left, PAUSED cleared, output storage and,
    /// unless a one-shot receive is armed, input storage released). Returns the held bid.
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id.
    pub(crate) fn start_drain(&mut self, id: ConnId) -> Option<u16> {
        let key = self.serving(id);
        let held = self.stop_service(key);
        let now_s = self.now_s;
        let r = self.record_mut(key);
        r.flags |= flag::DRAINING;
        r.touched = now_s;
        self.update_sweep(key);
        held
    }
    // The steps close_started and start_drain share: leave the ready list, the wait FIFO and the
    // re-arm FIFO, clear PAUSED, release the output storage and, unless ONCE is set, the input
    // storage, and hand back the held bid. The caller sets its flag and updates the sweep list.
    fn stop_service(&mut self, key: u32) -> Option<u16> {
        for kind in [Kind::Ready, Kind::Wait, Kind::Rearm] {
            self.unlink(key, kind);
        }
        if self.record(key).flags & flag::PAUSED != 0 {
            // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
            self.paused -= 1;
        }
        let r = self.record_mut(key);
        r.flags &= !flag::PAUSED;
        let held = if r.flags & flag::HELD != 0 {
            r.flags &= !flag::HELD;
            Some(r.held_bid)
        } else {
            None
        };
        let output = std::mem::take(&mut r.output);
        let input = if r.flags & flag::ONCE == 0 {
            std::mem::take(&mut r.input)
        } else {
            0
        };
        if input != 0 {
            self.release(input, BufferKind::Input);
        }
        if output != 0 {
            self.release(output, BufferKind::Output);
        }
        held
    }
    /// Starts a close whose state stays in this record: close_started, then LEAN and the tail of
    /// the lean close list. The caller has checked that no operation is in flight and nothing is
    /// unsent; returns the held bid as close_started does.
    /// # Panics
    /// Panics for an invalid/stale id, an already closing connection or an operation in flight.
    pub(crate) fn lean_close(&mut self, id: ConnId) -> Option<u16> {
        let key = self.checked(id, false);
        assert!(
            self.record(key).flags & (flag::RECEIVING | flag::ONCE | flag::POLLING | flag::SENDING)
                == 0,
            "lean close with an operation in flight"
        );
        let held = self.close_started(id);
        self.record_mut(key).flags |= flag::LEAN;
        self.link(key, Kind::Lean);
        held
    }
    /// The lean closes waiting for their next push (SIOCOUTQ or Close).
    pub(crate) fn lean_len(&self) -> u32 {
        self.lean.len
    }
    /// Takes the head of the lean close list, with its flags.
    pub(crate) fn lean_pop(&mut self) -> Option<(ConnId, u32)> {
        if self.lean.len == 0 {
            return None;
        }
        let key = self.lean.head;
        self.unlink(key, Kind::Lean);
        Some((self.id(key), self.record(key).flags))
    }
    /// Puts a lean close back at the tail of the list (a push to retry next turn).
    /// # Panics
    /// Panics for a stale id or a record that is not a lean close.
    pub(crate) fn lean_requeue(&mut self, id: ConnId) {
        let key = self.checked(id, true);
        assert!(self.record(key).flags & flag::LEAN != 0, "not a lean close");
        if self.record(key).flags & flag::LEAN_LIST == 0 {
            self.link(key, Kind::Lean);
        }
    }
    /// The SIOCOUTQ question of a lean close was pushed.
    /// # Panics
    /// Panics for a stale id or a record that is not a lean close.
    pub(crate) fn lean_asked(&mut self, id: ConnId) {
        let key = self.checked(id, true);
        assert!(self.record(key).flags & flag::LEAN != 0, "not a lean close");
        self.record_mut(key).flags |= flag::OUTQ_WAIT | flag::OUTQ_DONE;
    }
    /// A SIOCOUTQ answer. For a lean close waiting on it, records whether no byte is
    /// unacknowledged, puts it back on the list and returns true; any other id returns false.
    /// # Panics
    /// Panics for a lean close that was not waiting on its answer.
    pub(crate) fn lean_answer(&mut self, id: ConnId, unacked: Unacked) -> bool {
        let key = self.key(id.ring, id.slot);
        let r = self.record(key);
        if r.flags & flag::LIVE == 0 || r.generation != id.generation || r.flags & flag::LEAN == 0 {
            return false;
        }
        assert!(r.flags & flag::OUTQ_WAIT != 0, "lean answer not awaited");
        let r = self.record_mut(key);
        r.flags &= !flag::OUTQ_WAIT;
        if unacked == Unacked::Zero {
            r.flags |= flag::OUTQ_ZERO;
        }
        self.link(key, Kind::Lean);
        true
    }
    /// # Panics
    /// Panics for an invalid/stale id or an already closing connection.
    pub(crate) fn read_eof(&mut self, id: ConnId) {
        let key = self.checked(id, false);
        self.record_mut(key).flags |= flag::READ_EOF;
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id or a request already in service.
    pub(crate) fn begin_service(&mut self, id: ConnId) {
        let key = self.serving(id);
        assert!(
            self.record(key).flags & flag::IN_SERVICE == 0,
            "request already in service"
        );
        self.record_mut(key).flags =
            (self.record(key).flags | flag::IN_SERVICE) & !flag::CONTINUE_SENT;
    }
    fn input_key(&self, id: ConnId) -> u32 {
        let key = self.serving(id);
        assert!(
            self.record(key).flags & flag::ONCE == 0,
            "one-shot receive armed"
        );
        key
    }
    fn front(&mut self, index: u32) {
        let s = self.storage_mut(index);
        if s.start != 0 {
            let start = s.start as usize;
            s.bytes.copy_within(start.., 0);
            // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
            s.bytes.truncate(s.bytes.len() - start);
            s.start = 0;
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing id, an armed one-shot receive, or total
    /// outside 1..=INPUT_MAX or below the held input length.
    pub(crate) fn reserve_input(&mut self, id: ConnId, total: u32) -> bool {
        let key = self.input_key(id);
        let index = self.record(key).input;
        assert!(
            (1..=INPUT_MAX).contains(&total) && total >= self.held_len(index),
            "reservation out of range"
        );
        let old = if index == 0 {
            0
        } else {
            self.storage(index).charge
        };
        let growth = total.saturating_sub(old);
        // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
        if growth > 0 && (self.wait.len != 0 || self.in_charged + u64::from(growth) > INPUT_BUDGET)
        {
            return false;
        }
        self.drop_for_room(BufferKind::Input, u64::from(growth));
        let index = if index == 0 {
            let index = self.acquire(BufferKind::Input, total);
            self.record_mut(key).input = index;
            index
        } else {
            self.front(index);
            index
        };
        let before = self.storage(index).charge;
        let s = self.storage_mut(index);
        if s.bytes.capacity() < total as usize {
            s.bytes.reserve_exact(total as usize - s.bytes.len());
        } else if s.bytes.capacity() > total as usize {
            s.bytes.shrink_to(total as usize);
        }
        s.charge = u32::try_from(s.bytes.capacity()).expect("bounded input capacity");
        let after = s.charge;
        // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
        self.in_charged = self.in_charged - u64::from(before) + u64::from(after);
        self.update_sweep(key);
        true
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id or an armed one-shot receive.
    pub(crate) fn append_input(&mut self, id: ConnId, bytes: &[u8]) -> bool {
        let key = self.input_key(id);
        let index = self.record(key).input;
        if index == 0 {
            return false;
        }
        // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
        let room = self.storage(index).bytes.capacity() - (self.held_len(index) as usize);
        if room < bytes.len() {
            return false;
        }
        if self.storage(index).bytes.capacity() - self.storage(index).bytes.len() < bytes.len() {
            self.front(index);
        }
        self.storage_mut(index).bytes.extend_from_slice(bytes);
        if !bytes.is_empty() {
            self.record_mut(key).touched = self.now_s;
        }
        true
    }
    /// # Panics
    /// Panics for an invalid/stale id or an already closing connection.
    pub(crate) fn input(&self, id: ConnId) -> &[u8] {
        let key = self.checked(id, false);
        let index = self.record(key).input;
        if index == 0 {
            &[]
        } else {
            let s = self.storage(index);
            s.bytes
                .get(s.start as usize..)
                .expect("start <= len for every held storage")
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing id, an armed one-shot receive, or a
    /// rewrite extending past the held input.
    pub(crate) fn rewrite_input(&mut self, id: ConnId, at: u32, bytes: &[u8]) {
        let key = self.input_key(id);
        let index = self.record(key).input;
        assert!(
            (at as usize)
                .checked_add(bytes.len())
                .is_some_and(|n| n <= self.held_len(index) as usize),
            "input range out of range"
        );
        if !bytes.is_empty() {
            let s = self.storage_mut(index);
            let start = s.start as usize + at as usize;
            s.bytes
                .get_mut(start..start + bytes.len())
                .expect("at + bytes.len() <= input_len, asserted above")
                .copy_from_slice(bytes);
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing id, an armed one-shot receive, or a
    /// consumed length beyond the held input.
    pub(crate) fn consume_input(&mut self, id: ConnId, n: u32) {
        let key = self.input_key(id);
        let index = self.record(key).input;
        assert!(n <= self.held_len(index), "input range out of range");
        if index != 0 {
            self.storage_mut(index).start += n;
            if self.held_len(index) == 0 {
                self.record_mut(key).input = 0;
                self.release(index, BufferKind::Input);
                self.update_sweep(key);
            }
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing id or a one-shot already armed.
    pub(crate) fn start_once(&mut self, id: ConnId) -> Option<(NonNull<u8>, u32)> {
        let key = self.input_key(id);
        let index = self.record(key).input;
        if index == 0 {
            return None;
        }
        self.front(index);
        let s = self.storage_mut(index);
        let len = s.bytes.len();
        let room = s.bytes.capacity() - len;
        if room == 0 {
            return None;
        }
        let pointer = NonNull::new(s.bytes.spare_capacity_mut().as_mut_ptr().cast())
            .expect("allocated input spare capacity");
        self.record_mut(key).flags |= flag::ONCE;
        Some((
            pointer,
            u32::try_from(room).expect("bounded input capacity"),
        ))
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id or a zero reservation.
    pub(crate) fn reserve_output(&mut self, id: ConnId, n: u32) -> bool {
        let key = self.serving(id);
        assert!(n >= 1, "reservation out of range");
        let index = self.record(key).output;
        let (len, old) = if index == 0 {
            (0, 0)
        } else {
            let s = self.storage(index);
            (s.bytes.len(), s.bytes.capacity())
        };
        let needed = len
            .checked_add(n as usize)
            .expect("output range out of range");
        let growth = needed.saturating_sub(old);
        // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
        if self.out_charged + growth as u64 > OUTPUT_BUDGET {
            return false;
        }
        self.drop_for_room(BufferKind::Output, growth as u64);
        if index == 0 {
            let index = self.acquire(BufferKind::Output, n);
            self.record_mut(key).output = index;
        } else if growth != 0 {
            let s = self.storage_mut(index);
            s.bytes.reserve_exact(n as usize);
            s.charge = u32::try_from(s.bytes.capacity()).expect("bounded output capacity");
            // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
            self.out_charged += u64::from(s.charge) - old as u64;
        }
        self.update_sweep(key);
        true
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id or missing output reservation.
    pub(crate) fn output(&mut self, id: ConnId) -> &mut Vec<u8> {
        let key = self.serving(id);
        let index = self.record(key).output;
        assert!(index != 0, "output range out of range");
        &mut self.storage_mut(index).bytes
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id, invalid reply ranges, missing
    /// output storage, or capacity changed outside reserve_output.
    pub(crate) fn commit_output(&mut self, id: ConnId, base: u32, start: u32, reply: Reply) {
        let key = self.serving(id);
        let index = self.record(key).output;
        assert!(index != 0, "output range out of range");
        let s = self.storage_mut(index);
        assert!(
            base <= start && start as usize <= s.bytes.len(),
            "output range out of range"
        );
        assert!(
            s.bytes.capacity() == s.charge as usize,
            "output capacity changed outside reserve_output"
        );
        if s.start == base {
            s.start = start;
        } else {
            s.bytes.copy_within(start as usize.., base as usize);
            s.bytes.truncate(s.bytes.len() - (start - base) as usize);
        }
        self.record_mut(key).touched = self.now_s;
        match reply {
            Reply::Continue => self.record_mut(key).flags |= flag::CONTINUE_SENT,
            Reply::Final { close } => {
                self.record_mut(key).flags &= !(flag::IN_SERVICE | flag::CONTINUE_SENT);
                if close {
                    self.record_mut(key).flags |= flag::CLOSE_AFTER;
                } else if self.held_len(self.record(key).input) > 0
                    && self.record(key).flags & (flag::PAUSED | flag::READY) == 0
                {
                    self.link(key, Kind::Ready);
                }
            }
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id, a send already in flight, or
    /// output capacity changed outside reserve_output.
    pub(crate) fn start_send(&mut self, id: ConnId) -> Option<(NonNull<u8>, u32)> {
        let key = self.serving(id);
        assert!(
            self.record(key).flags & flag::SENDING == 0,
            "send in flight"
        );
        if self.held_len(self.record(key).flight) == 0 {
            let output = self.record(key).output;
            if output == 0 || self.held_len(output) == 0 {
                return None;
            }
            let s = self.storage(output);
            assert!(
                s.bytes.capacity() == s.charge as usize,
                "output capacity changed outside reserve_output"
            );
            self.record_mut(key).flight = output;
            self.record_mut(key).output = 0;
        }
        let index = self.record(key).flight;
        let s = self.storage_mut(index);
        let start = s.start as usize;
        let pointer = NonNull::new(
            s.bytes
                .get_mut(start..)
                .expect("start <= len for every held storage")
                .as_mut_ptr(),
        )
        .expect("nonempty flight storage");
        // UNSAFE PATH (speed): unchecked arithmetic; every held storage keeps start <= length <= capacity == charge with capacities under the 32 MiB budget, so these offsets cannot wrap
        let len = u32::try_from(s.bytes.len() - start).expect("bounded flight storage");
        self.record_mut(key).flags |= flag::SENDING;
        Some((pointer, len))
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id, invalid input-wait reservation,
    /// an existing input reservation, or an input waiter/held bid already set.
    pub(crate) fn pause(&mut self, id: ConnId, cause: Pause) {
        let key = self.serving(id);
        if let Pause::Input { want, bid, len } = cause {
            assert!((1..=INPUT_MAX).contains(&want), "reservation out of range");
            assert!(self.record(key).input == 0, "input already reserved");
            assert!(
                self.record(key).flags & (flag::WAITING | flag::HELD) == 0,
                "operation already armed"
            );
            let r = self.record_mut(key);
            r.want = want;
            r.held_bid = bid;
            r.held_len = len;
            r.flags |= flag::HELD;
            self.link(key, Kind::Wait);
        }
        if self.record(key).flags & flag::PAUSED == 0 {
            // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
            self.paused += 1;
            self.record_mut(key).flags |= flag::PAUSED;
        }
    }
    /// # Panics
    /// Panics for an invalid/stale/closing/draining id, an unpaused connection, or an
    /// input waiter whose reservation has not been granted.
    pub(crate) fn resume(&mut self, id: ConnId) {
        let key = self.serving(id);
        assert!(
            self.record(key).flags & flag::PAUSED != 0
                && self.record(key).flags & flag::WAITING == 0,
            "operation not armed"
        );
        self.record_mut(key).flags &= !flag::PAUSED;
        // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
        self.paused -= 1;
        if self.record(key).flags
            & (flag::RECEIVING | flag::POLLING | flag::ONCE | flag::READ_EOF | flag::REARM)
            == 0
        {
            self.link(key, Kind::Rearm);
        }
        if self.held_len(self.record(key).input) > 0
            && self.record(key).flags & (flag::IN_SERVICE | flag::READY) == 0
        {
            self.link(key, Kind::Ready);
        }
    }
    /// # Panics
    /// Panics for an invalid/stale id or an already closing connection.
    pub(crate) fn queue_rearm(&mut self, id: ConnId) {
        let key = self.checked(id, false);
        if self.record(key).flags & flag::REARM == 0 {
            self.link(key, Kind::Rearm);
        }
    }
    /// # Panics
    /// Panics for a ring outside the eight allocated tables.
    pub(crate) fn take_rearm(&mut self, ring: u8, max: u32, mut f: impl FnMut(ConnId)) {
        assert!(usize::from(ring) < 8, "ring index out of range");
        let mut taken = 0;
        let table = |conns: &Self| {
            conns
                .tables
                .get(usize::from(ring))
                .expect("ring < 8, asserted above")
                .rearm
        };
        while taken < max && table(self).len != 0 {
            let key = table(self).head;
            self.unlink(key, Kind::Rearm);
            if self.record(key).flags
                & (flag::CLOSING
                    | flag::PAUSED
                    | flag::READ_EOF
                    | flag::RECEIVING
                    | flag::POLLING
                    | flag::ONCE)
                == 0
            {
                f(self.id(key));
                taken += 1;
            }
        }
    }
    pub(crate) fn take_granted(&mut self, mut f: impl FnMut(ConnId, u16, u16)) {
        while self.wait.len > 0 {
            let key = self.wait.head;
            let want = self.record(key).want;
            // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
            if self.in_charged + u64::from(want) > INPUT_BUDGET {
                break;
            }
            self.drop_for_room(BufferKind::Input, u64::from(want));
            let input = self.acquire(BufferKind::Input, want);
            self.record_mut(key).input = input;
            self.unlink(key, Kind::Wait);
            let r = self.record_mut(key);
            r.flags &= !(flag::PAUSED | flag::HELD);
            let (bid, len) = (r.held_bid, r.held_len);
            // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
            self.paused -= 1;
            self.update_sweep(key);
            f(self.id(key), bid, len);
        }
    }
    pub(crate) fn take_ready(&mut self, max: u32, mut f: impl FnMut(ConnId)) {
        let mut taken = 0;
        while taken < max && self.ready.len > 0 {
            let key = self.ready.head;
            self.unlink(key, Kind::Ready);
            if self.record(key).flags & (flag::CLOSING | flag::PAUSED | flag::IN_SERVICE) == 0
                && self.held_len(self.record(key).input) > 0
            {
                f(self.id(key));
                taken += 1;
            }
        }
    }
    pub(crate) fn sweep(&self, now_s: u32, mut f: impl FnMut(ConnId)) {
        for table in &self.tables {
            let mut key = table.sweep.head;
            while key != NONE {
                let r = self.record(key);
                if r.flags & flag::CLOSING == 0 && now_s.saturating_sub(r.touched) >= STALL_SECS {
                    f(self.id(key));
                }
                key = self.links(key, Kind::Sweep).1;
            }
        }
    }
    fn update_sweep(&mut self, key: u32) {
        let r = self.record(key);
        if r.input != 0 || r.output != 0 || r.flight != 0 || r.flags & flag::DRAINING != 0 {
            if r.flags & flag::SWEEP == 0 {
                self.link(key, Kind::Sweep);
            }
        } else {
            self.unlink(key, Kind::Sweep);
        }
    }
    fn list(&self, key: u32, kind: Kind) -> List {
        match kind {
            Kind::Ready => self.ready,
            Kind::Wait => self.wait,
            Kind::Rearm => {
                self.tables
                    .get((key >> 16) as usize)
                    .expect("key >> 16 < 8 for the per-ring lists")
                    .rearm
            }
            Kind::Sweep => {
                self.tables
                    .get((key >> 16) as usize)
                    .expect("key >> 16 < 8 for the per-ring lists")
                    .sweep
            }
            Kind::Lean => self.lean,
        }
    }
    fn set_list(&mut self, key: u32, kind: Kind, list: List) {
        match kind {
            Kind::Ready => self.ready = list,
            Kind::Wait => self.wait = list,
            Kind::Rearm => {
                self.tables
                    .get_mut((key >> 16) as usize)
                    .expect("key >> 16 < 8 for the per-ring lists")
                    .rearm = list;
            }
            Kind::Sweep => {
                self.tables
                    .get_mut((key >> 16) as usize)
                    .expect("key >> 16 < 8 for the per-ring lists")
                    .sweep = list;
            }
            Kind::Lean => self.lean = list,
        }
    }
    fn links(&self, key: u32, kind: Kind) -> (u32, u32) {
        let r = self.record(key);
        let expand = |slot: u16| {
            if slot == u16::MAX {
                NONE
            } else {
                (key & 0xffff0000) | u32::from(slot)
            }
        };
        match kind {
            Kind::Ready | Kind::Lean => (r.ready_prev, r.ready_next),
            Kind::Wait => (r.wait_prev, r.wait_next),
            Kind::Rearm => (expand(r.rearm_prev), expand(r.rearm_next)),
            Kind::Sweep => (expand(r.sweep_prev), expand(r.sweep_next)),
        }
    }
    fn set_links(&mut self, key: u32, kind: Kind, prev: u32, next: u32) {
        let r = self.record_mut(key);
        let slot = |key: u32| u16::try_from(key & 65535).expect("list slot");
        match kind {
            Kind::Ready | Kind::Lean => {
                r.ready_prev = prev;
                r.ready_next = next;
            }
            Kind::Wait => {
                r.wait_prev = prev;
                r.wait_next = next;
            }
            Kind::Rearm => {
                r.rearm_prev = slot(prev);
                r.rearm_next = slot(next);
            }
            Kind::Sweep => {
                r.sweep_prev = slot(prev);
                r.sweep_next = slot(next);
            }
        }
    }
    fn link(&mut self, key: u32, kind: Kind) {
        let mut list = self.list(key, kind);
        self.set_links(key, kind, list.tail, NONE);
        if list.tail == NONE {
            list.head = key;
        } else {
            let prev = self.links(list.tail, kind).0;
            self.set_links(list.tail, kind, prev, key);
        }
        list.tail = key;
        // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
        list.len += 1;
        self.set_list(key, kind, list);
        self.record_mut(key).flags |= kind.flag();
    }
    fn unlink(&mut self, key: u32, kind: Kind) {
        if self.record(key).flags & kind.flag() == 0 {
            return;
        }
        let mut list = self.list(key, kind);
        let (prev, next) = self.links(key, kind);
        if prev == NONE {
            list.head = next;
        } else {
            let before = self.links(prev, kind).0;
            self.set_links(prev, kind, before, next);
        }
        if next == NONE {
            list.tail = prev;
        } else {
            let after = self.links(next, kind).1;
            self.set_links(next, kind, prev, after);
        }
        // UNSAFE PATH (speed): unchecked arithmetic; this count moves by one per record transition and stays between 0 and the 524,280 records of 8 rings of 65,535 slots
        list.len -= 1;
        self.set_list(key, kind, list);
        self.set_links(key, kind, NONE, NONE);
        self.record_mut(key).flags &= !kind.flag();
    }
    fn totals(&mut self, kind: BufferKind) -> (&mut u64, &mut u64, &mut Vec<u32>) {
        match kind {
            BufferKind::Input => (&mut self.in_charged, &mut self.in_pooled, &mut self.kept_in),
            BufferKind::Output => (
                &mut self.out_charged,
                &mut self.out_pooled,
                &mut self.kept_out,
            ),
        }
    }
    fn budget(kind: BufferKind) -> u64 {
        match kind {
            BufferKind::Input => INPUT_BUDGET,
            BufferKind::Output => OUTPUT_BUDGET,
        }
    }
    fn drop_for_room(&mut self, kind: BufferKind, growth: u64) {
        loop {
            let (charged, pooled, kept) = self.totals(kind);
            // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
            if *charged + *pooled + growth <= Self::budget(kind) {
                break;
            }
            let Some(index) = kept.pop() else { break };
            let capacity = self.storage(index).bytes.capacity() as u64;
            // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
            *self.totals(kind).1 -= capacity;
            self.storage_mut(index).bytes = Vec::new();
            self.empty.push(index);
        }
    }
    fn acquire(&mut self, kind: BufferKind, need: u32) -> u32 {
        let top = self.totals(kind).2.last().copied();
        if let Some(index) = top {
            let capacity = self.storage(index).bytes.capacity();
            if capacity >= need as usize {
                self.totals(kind).2.pop();
                // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
                *self.totals(kind).1 -= capacity as u64;
                *self.totals(kind).0 += capacity as u64;
                self.storage_mut(index).charge =
                    u32::try_from(capacity).expect("bounded pooled buffer");
                return index;
            }
        }
        let index = self.empty.pop().unwrap_or_else(|| {
            assert!(
                self.arena.len() < self.arena.capacity(),
                "storage arena bound"
            );
            self.arena.push(Storage {
                bytes: Vec::new(),
                start: 0,
                charge: 0,
            });
            u32::try_from(self.arena.len()).expect("storage index")
        });
        let s = self.storage_mut(index);
        s.bytes = Vec::with_capacity(need as usize);
        s.start = 0;
        s.charge = u32::try_from(s.bytes.capacity()).expect("bounded allocated buffer");
        let charge = s.charge;
        // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
        *self.totals(kind).0 += u64::from(charge);
        index
    }
    fn release(&mut self, index: u32, kind: BufferKind) {
        let s = self.storage_mut(index);
        let charge = s.charge;
        let capacity = s.bytes.capacity();
        s.bytes.clear();
        s.start = 0;
        s.charge = 0;
        // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
        *self.totals(kind).0 -= u64::from(charge);
        if capacity <= KEEP_MAX as usize && self.totals(kind).2.len() < KEEP_COUNT as usize {
            self.totals(kind).2.push(index);
            // UNSAFE PATH (speed): unchecked arithmetic; the charged and pooled totals are sums of held and kept capacities, at most the 32 MiB budget, so these u64 sums and differences cannot wrap
            *self.totals(kind).1 += capacity as u64;
        } else {
            self.storage_mut(index).bytes = Vec::new();
            self.empty.push(index);
        }
    }
}
