use io_uring::{IoUring, Probe, cqueue, opcode, squeue, types};
use std::io::{self, Write};
use std::mem::ManuallyDrop;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU16, Ordering};
use std::time::{Duration, Instant};
#[cfg(twelve_fault_inject)]
use std::{fs::File, io::Read, path::PathBuf};

pub(crate) const RINGS: usize = 8;
pub(crate) const BUF_SIZE: u32 = 2_048;
pub(crate) const PROBED: [&str; 10] = [
    "IORING_OP_ACCEPT",
    "IORING_OP_RECV",
    "IORING_OP_SEND",
    "IORING_OP_CLOSE",
    "IORING_OP_ASYNC_CANCEL",
    "IORING_OP_POLL_ADD",
    "IORING_OP_FSYNC",
    "IORING_FEAT_NODROP",
    "IORING_FEAT_EXT_ARG",
    "IORING_FEAT_FAST_POLL",
];
const SQ_ENTRIES: u32 = if cfg!(twelve_small_limits) { 8 } else { 4_096 };
const GETEVENTS: u32 = 1;
const EXT_ARG: u32 = 1 << 3;
const REGISTERED_RING: u32 = 1 << 4;
const NO_INDEX: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[must_use]
pub(crate) enum Submit<T> {
    Done(T),
    RetryNextTurn,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum RecvMode {
    Multishot,
    Single,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Armed {
    Recv,
    Poll,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum CloseLinger {
    Keep,
    Zero,
    Off,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum CloseIo {
    Quiet,
    Pending,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum WaitEnd {
    Ready,
    Count,
    Timeout,
    Retry,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct Reaped {
    pub(crate) count: u32,
    pub(crate) left: bool,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct RingCounts {
    pub(crate) ring_enters: u64,
    pub(crate) enter_retries: u64,
    pub(crate) sq_full: u64,
    pub(crate) cq_overflow: u64,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct Startup {
    pub(crate) rings: u8,
    pub(crate) slots_per_ring: u32,
    pub(crate) bufs_per_ring: u16,
    pub(crate) pbuf_retry: bool,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Completion {
    Accept {
        ring: u8,
        arm: u32,
        result: i32,
        more: bool,
    },
    Recv {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
        buffer: Option<u16>,
        more: bool,
    },
    RecvOnce {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
    Poll {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
        more: bool,
    },
    Send {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
    Cancel {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
    AcceptCancel {
        ring: u8,
        arm: u32,
        result: i32,
    },
    Close {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
    OrphanClose {
        ring: u8,
        slot: u16,
        result: i32,
    },
    Fsync {
        result: i32,
    },
    /// The socket's unacknowledged send bytes (SIOCOUTQ, write_seq - snd_una) or a negative errno.
    OutQ {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
    /// The SO_LINGER SetSockOpt hardlinked before a Close: 0 or a negative errno.
    Linger {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
    /// The write side's shutdown (SHUT_WR): 0 or a negative errno.
    Shutdown {
        ring: u8,
        slot: u16,
        generation: u32,
        result: i32,
    },
}

#[repr(C)]
struct BufReg {
    ring_addr: u64,
    ring_entries: u32,
    bgid: u16,
    flags: u16,
    resv: [u64; 3],
}
const _: () = assert!(size_of::<BufReg>() == 40);
const _: () = assert!(std::mem::offset_of!(BufReg, ring_addr) == 0);
const _: () = assert!(std::mem::offset_of!(BufReg, ring_entries) == 8);
const _: () = assert!(std::mem::offset_of!(BufReg, bgid) == 12);
const _: () = assert!(std::mem::offset_of!(BufReg, flags) == 14);
const _: () = assert!(std::mem::offset_of!(BufReg, resv) == 16);
#[repr(C)]
struct ResourceUpdate {
    offset: u32,
    resv: u32,
    data: u64,
}
const _: () = assert!(size_of::<ResourceUpdate>() == 16);
const _: () = assert!(std::mem::offset_of!(ResourceUpdate, offset) == 0);
const _: () = assert!(std::mem::offset_of!(ResourceUpdate, resv) == 4);
const _: () = assert!(std::mem::offset_of!(ResourceUpdate, data) == 8);

struct Mapping {
    address: NonNull<libc::c_void>,
    len: usize,
}
impl Mapping {
    fn new(len: usize) -> io::Result<Self> {
        // SAFETY: anonymous private mapping needs no backing fd or address. A
        // successful mapping is owned here and unmapped once with the same size.
        let address = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        if address == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        Ok(Self {
            address: NonNull::new(address).expect("non-null anonymous mapping"),
            len,
        })
    }
}
impl Drop for Mapping {
    fn drop(&mut self) {
        // SAFETY: munmap needs a live extent that nothing uses later: this is the original mmap
        // extent, dropped once; Kernel drops only after every operation ended and its buffer ring
        // was unregistered, and Building only before any SQE existed.
        let _ = unsafe { libc::munmap(self.address.as_ptr(), self.len) };
    }
}

struct Ring {
    io: IoUring,
    index: u32,
    in_flight: u32,
    held: u32,
    held_bits: Vec<u64>,
    tail: u16,
    published: u16,
    arm: u32,
}

// The kernel-visible resources: Rings drops them only on its release path and leaks them otherwise.
struct Kernel {
    rings: [Ring; RINGS],
    mappings: [[Mapping; 2]; RINGS],
    // The SO_LINGER values a SetSockOpt SQE points at, [CloseLinger::Zero, CloseLinger::Off]: on
    // the heap, a resident anonymous page, where a static's read-only data page could be evicted
    // and faulted back in by a close at the memory wall; the kernel reads them only through SQEs.
    lingers: Box<[libc::linger; 2]>,
}

pub(crate) struct Rings {
    // Dropped once by Drop's release path, never by its leak path.
    kernel: ManuallyDrop<Kernel>,
    slots_per_ring: u32,
    bufs_per_ring: u16,
    start: u8,
    pbuf_retry: bool,
    counts: RingCounts,
    #[cfg(twelve_fault_inject)]
    _pbuf_path: Option<PathBuf>,
    #[cfg(twelve_fault_inject)]
    enter_path: Option<PathBuf>,
}

// Startup rollback owns only completed registrations. No SQE exists yet.
struct Building {
    rings: [Option<Ring>; RINGS],
    maps: [[Option<Mapping>; 2]; RINGS],
    files: [bool; RINGS],
    pbuf: [bool; RINGS],
}
impl Drop for Building {
    fn drop(&mut self) {
        let steps = self
            .rings
            .iter_mut()
            .zip(self.maps.iter_mut())
            .zip(self.files)
            .zip(self.pbuf);
        for (((slot, [memory, descriptors]), files), pbuf) in steps.rev() {
            if let Some(ring) = slot.as_ref() {
                if pbuf {
                    unregister_pbuf(ring.io.as_fd());
                }
                drop(descriptors.take());
                drop(memory.take());
                if files {
                    let _ = ring.io.submitter().unregister_files();
                }
                if ring.index != NO_INDEX {
                    unregister_index(ring.io.as_fd(), ring.index);
                }
            }
            drop(slot.take());
        }
    }
}

impl Rings {
    /// # Errors
    /// Rejects invalid sizes, unavailable kernel features and setup/registration
    /// failures, undoing every completed startup step before returning.
    pub(crate) fn new(slots_per_ring: u32, bufs_per_ring: u16) -> io::Result<Self> {
        if !(16..=65_535).contains(&slots_per_ring) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("io_uring: slots_per_ring {slots_per_ring} outside 16..=65535"),
            ));
        }
        if !bufs_per_ring.is_power_of_two() || bufs_per_ring > 32_768 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "io_uring: bufs_per_ring {bufs_per_ring} is not a power of two in 1..=32768"
                ),
            ));
        }
        #[cfg(twelve_fault_inject)]
        let (pbuf_path, enter_path) = {
            let base = std::env::var_os("SQLITE_PATH")
                .filter(|base| !base.as_encoded_bytes().contains(&0));
            let path = |suffix: &str| {
                base.as_ref().map(|base| {
                    let mut name = base.clone();
                    name.push(suffix);
                    PathBuf::from(name)
                })
            };
            (path("-fault-pbuf-einval"), path("-fault-enter"))
        };
        let mut building = Building {
            rings: std::array::from_fn(|_| None),
            maps: std::array::from_fn(|_| [None, None]),
            files: [false; RINGS],
            pbuf: [false; RINGS],
        };
        let mut pbuf_retry = false;
        for r in 0..RINGS {
            let io = IoUring::builder()
                .setup_cqsize(16_384)
                .setup_single_issuer()
                .setup_coop_taskrun()
                .setup_defer_taskrun()
                // IORING_SQ_TASKRUN marks a ring whose deferred completions wait for an enter, so
                // wait() enters only rings with work.
                .setup_taskrun_flag()
                .build(SQ_ENTRIES)
                .map_err(|e| contextual("setup ring", r, e))?;
            let ring = building
                .rings
                .get_mut(r)
                .expect("r < RINGS in the setup loop")
                .insert(Ring {
                    io,
                    index: NO_INDEX,
                    in_flight: 0,
                    held: 0,
                    held_bits: vec![0; usize::from(bufs_per_ring).div_ceil(64)],
                    tail: 0,
                    published: 0,
                    arm: 0,
                });
            let mut update = ResourceUpdate {
                offset: u32::MAX,
                resv: 0,
                data: u64::try_from(ring.io.as_raw_fd()).expect("nonnegative ring fd"),
            };
            // SAFETY: the fd is this thread's live ring descriptor, owned by ring.io; update is an
            // owned local with io_uring_rsrc_update's asserted layout, writable through &raw mut
            // for the offset IORING_REGISTER_RING_FDS fills; this registration names no memory.
            unsafe { register(ring.io.as_raw_fd(), 20, (&raw mut update).cast(), 1) }
                .map_err(|e| contextual("register ring fd", r, e))?;
            ring.index = update.offset;
            if r == 0 {
                let mut probe = Probe::new();
                ring.io
                    .submitter()
                    .register_probe(&mut probe)
                    .map_err(|e| contextual("probe ring", 0, e))?;
                let codes = [
                    opcode::AcceptMulti::CODE,
                    opcode::RecvMulti::CODE,
                    opcode::Send::CODE,
                    opcode::Close::CODE,
                    opcode::AsyncCancel::CODE,
                    opcode::PollAdd::CODE,
                    opcode::Fsync::CODE,
                ];
                for (name, code) in PROBED.iter().zip(codes) {
                    if !probe.is_supported(code) {
                        return Err(missing(name));
                    }
                }
                let params = ring.io.params();
                let features = [
                    params.is_feature_nodrop(),
                    params.is_feature_ext_arg(),
                    params.is_feature_fast_poll(),
                ];
                for (name, present) in PROBED.iter().skip(codes.len()).zip(features) {
                    if !present {
                        return Err(missing(name));
                    }
                }
            }
            ring.io
                .submitter()
                .register_files_sparse(slots_per_ring)
                .map_err(|e| contextual("register files", r, e))?;
            *building
                .files
                .get_mut(r)
                .expect("r < RINGS in the setup loop") = true;
            let [memory, descriptors] = building
                .maps
                .get_mut(r)
                .expect("r < RINGS in the setup loop");
            let memory = &*memory.insert(
                Mapping::new(usize::from(bufs_per_ring) * BUF_SIZE as usize)
                    .map_err(|e| contextual("map buffers", r, e))?,
            );
            let descriptors = &*descriptors.insert(
                Mapping::new(usize::from(bufs_per_ring) * 16)
                    .map_err(|e| contextual("map buffers", r, e))?,
            );
            let mut reg = BufReg {
                ring_addr: descriptors.address.as_ptr().addr() as u64,
                ring_entries: u32::from(bufs_per_ring),
                bgid: 0,
                flags: 0,
                resv: [0; 3],
            };
            #[cfg(twelve_fault_inject)]
            let shim = pbuf_path
                .as_ref()
                .is_some_and(|path| std::fs::metadata(path).is_ok());
            #[cfg(not(twelve_fault_inject))]
            let shim = false;
            let retried = register_pbuf(&mut reg, |reg| {
                if shim && reg.resv == [0; 3] {
                    return Err(io::Error::from_raw_os_error(libc::EINVAL));
                }
                let mut submitted = BufReg {
                    resv: if shim { [0; 3] } else { reg.resv },
                    ..*reg
                };
                // SAFETY: the fd is this thread's live ring descriptor, owned by ring.io; submitted
                // is an owned local with io_uring_buf_reg's asserted layout, passed through
                // &raw mut; the descriptor ring it names stays mapped in `building` until the
                // buffer ring is unregistered (Building's Drop or Rings' release path).
                unsafe { register(ring.io.as_raw_fd(), 22, (&raw mut submitted).cast(), 1) }
            })
            .map_err(|e| contextual("register buffer ring", r, e))?;
            *building
                .pbuf
                .get_mut(r)
                .expect("r < RINGS in the setup loop") = true;
            pbuf_retry |= retried;
            for bid in 0..bufs_per_ring {
                // SAFETY: descriptors and memory are this ring's mappings of bufs_per_ring entries
                // and buffers, a power of two validated above; bid < bufs_per_ring, and nothing is
                // published before the publish below, so the kernel can select no slot yet.
                unsafe { descriptor(descriptors, memory, bid, bid, bufs_per_ring) };
            }
            ring.tail = bufs_per_ring;
            publish(ring, descriptors);
        }
        let rings = building
            .rings
            .each_mut()
            .map(|ring| ring.take().expect("the setup loop built all eight rings"));
        let mappings = building.maps.each_mut().map(|[memory, descriptors]| {
            [
                memory
                    .take()
                    .expect("the setup loop mapped every buffer memory"),
                descriptors
                    .take()
                    .expect("the setup loop mapped every descriptor ring"),
            ]
        });
        Ok(Self {
            kernel: ManuallyDrop::new(Kernel {
                rings,
                mappings,
                lingers: Box::new([
                    libc::linger {
                        l_onoff: 1,
                        l_linger: 0,
                    },
                    libc::linger {
                        l_onoff: 0,
                        l_linger: 0,
                    },
                ]),
            }),
            slots_per_ring,
            bufs_per_ring,
            start: 0,
            pbuf_retry,
            counts: RingCounts::default(),
            #[cfg(twelve_fault_inject)]
            _pbuf_path: pbuf_path,
            #[cfg(twelve_fault_inject)]
            enter_path,
        })
    }
    fn ring_index(&self, ring: u8) -> usize {
        assert!(usize::from(ring) < RINGS, "ring index out of range");
        usize::from(ring)
    }
    fn slot(&self, ring: u8, slot: u16) -> usize {
        let r = self.ring_index(ring);
        assert!(u32::from(slot) < self.slots_per_ring, "slot out of range");
        r
    }
    fn state(&self, r: usize) -> &Ring {
        self.kernel.rings.get(r).expect(
            "r < RINGS: every caller passes a ring_index or slot result or a loop index below RINGS",
        )
    }
    fn state_mut(&mut self, r: usize) -> &mut Ring {
        self.kernel.rings.get_mut(r).expect(
            "r < RINGS: every caller passes a ring_index or slot result or a loop index below RINGS",
        )
    }
    fn pending(&mut self, r: usize) -> u32 {
        u32::try_from(self.state_mut(r).io.submission().len()).expect("bounded SQ")
    }
    fn room(&mut self, r: usize) -> usize {
        let sq = self.state_mut(r).io.submission();
        sq.capacity() - sq.len()
    }
    fn enter(
        &mut self,
        r: usize,
        pending: u32,
        minimum: u32,
        flags: u32,
        args: Option<&types::SubmitArgs<'_, '_>>,
        inject: bool,
    ) -> io::Result<u32> {
        #[cfg(twelve_fault_inject)]
        if inject && let Some(errno) = self.injected_errno() {
            return Err(io::Error::from_raw_os_error(errno));
        }
        #[cfg(not(twelve_fault_inject))]
        let _ = inject;
        let kernel = &mut *self.kernel;
        let ring = kernel.rings.get_mut(r).expect("r < RINGS from the caller");
        let [_, descriptors] = kernel.mappings.get(r).expect("r < RINGS from the caller");
        publish(ring, descriptors);
        if r != 0 {
            self.counts.ring_enters = self.counts.ring_enters.saturating_add(1);
        }
        let (arg, size) = match args {
            Some(args) => (
                (args as *const types::SubmitArgs<'_, '_>).cast::<libc::c_void>(),
                size_of::<types::SubmitArgs<'_, '_>>(),
            ),
            None => (std::ptr::null(), size_of::<u64>()),
        };
        // SAFETY: registered index belongs to this same thread. SubmitArgs is
        // repr(transparent) over the kernel ABI and its Timespec outlives this
        // call; a null sigmask otherwise carries the kernel sigset size.
        let result = unsafe {
            libc::syscall(
                libc::SYS_io_uring_enter,
                ring.index,
                pending,
                minimum,
                flags | REGISTERED_RING,
                arg,
                size,
            )
        };
        if result < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(u32::try_from(result).expect("enter submission count"))
        }
    }
    fn retry(&mut self, error: &io::Error) -> bool {
        if matches!(
            error.raw_os_error(),
            Some(libc::EINTR | libc::EAGAIN | libc::EBUSY | libc::ENOMEM)
        ) {
            self.counts.enter_retries = self.counts.enter_retries.saturating_add(1);
            true
        } else {
            false
        }
    }
    /// Pushes `entries` contiguously; a full SQ waits for the next round's one enter.
    ///
    /// # Safety
    /// Every entry's parameters must stay valid for the whole kernel operation: memory an entry
    /// points to (a recv_once or send buffer, a SetSockOpt value, a provided buffer) stays
    /// allocated and unmoved, and untouched by Rust code where the kernel reads or writes it,
    /// until that entry's terminal completion is reaped or drain returns, and for the rest of the
    /// process when this Rings is dropped without drain while the entry is in flight.
    unsafe fn push(&mut self, r: usize, entries: &[squeue::Entry]) -> io::Result<Submit<()>> {
        if self.room(r) < entries.len() {
            self.counts.sq_full = self.counts.sq_full.saturating_add(1);
            return Ok(Submit::RetryNextTurn);
        }
        let ring = self.state_mut(r);
        // SAFETY: push_multiple needs every entry's parameters valid for the whole operation,
        // which this function's contract leaves to its caller; the room check above found space
        // for every entry.
        unsafe { ring.io.submission().push_multiple(entries) }.expect("checked SQ room");
        // UNSAFE PATH (speed): unchecked arithmetic; each in-flight SQE is a live kernel request of at least 64 bytes, so in_flight cannot reach u32::MAX on a 2 GB box
        ring.in_flight += u32::try_from(entries.len()).expect("at most three entries");
        Ok(Submit::Done(()))
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for a ring index outside the eight owned rings.
    pub(crate) fn arm_accept(
        &mut self,
        ring: u8,
        listener: BorrowedFd<'_>,
    ) -> io::Result<Submit<u32>> {
        let r = self.ring_index(ring);
        let arm = self.state(r).arm.wrapping_add(1);
        let entry = opcode::AcceptMulti::new(types::Fd(listener.as_raw_fd()))
            .allocate_file_index(true)
            .build()
            .user_data(encode(Tag::Accept, 0, arm));
        // SAFETY: the AcceptMulti entry names the listener by descriptor and points at no memory.
        match unsafe { self.push(r, &[entry]) }? {
            Submit::Done(()) => {
                self.state_mut(r).arm = arm;
                Ok(Submit::Done(arm))
            }
            Submit::RetryNextTurn => Ok(Submit::RetryNextTurn),
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for a ring index outside the eight owned rings.
    pub(crate) fn cancel_accept(&mut self, ring: u8, arm: u32) -> io::Result<Submit<()>> {
        let r = self.ring_index(ring);
        // SAFETY: the AsyncCancel entry names its target by user data and points at no memory.
        unsafe {
            self.push(
                r,
                &[opcode::AsyncCancel::new(encode(Tag::Accept, 0, arm))
                    .build()
                    .user_data(encode(Tag::AcceptCancel, 0, arm))],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    pub(crate) fn arm_recv(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
        mode: RecvMode,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        let fd = types::Fixed(u32::from(slot));
        let entry = match mode {
            RecvMode::Multishot => opcode::RecvMulti::new(fd, 0).build(),
            RecvMode::Single => opcode::Recv::new(fd, std::ptr::null_mut(), BUF_SIZE)
                .buf_group(0)
                .build()
                .flags(squeue::Flags::BUFFER_SELECT),
        }
        .user_data(encode(Tag::Recv, slot, generation));
        // SAFETY: the receive points at no memory of its own: it takes one of this ring's provided
        // buffers, whose mapping lives as long as the Kernel, which Drop leaks rather than frees
        // while the receive is in flight.
        unsafe { self.push(r, &[entry]) }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    pub(crate) fn arm_poll(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        // SAFETY: the PollAdd entry names a fixed slot and points at no memory.
        unsafe {
            self.push(
                r,
                &[opcode::PollAdd::new(
                    types::Fixed(u32::from(slot)),
                    u32::try_from(libc::POLLIN).expect("positive POLLIN"),
                )
                .multi(true)
                .build()
                .user_data(encode(Tag::Poll, slot, generation))],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    /// # Safety
    /// When the call returns Ok(Submit::Done(())), buf must point to len bytes that stay
    /// allocated, unmoved and unread by Rust code until this SQE's terminal completion is reaped,
    /// by reap or by drain; when this Rings is dropped without drain while the SQE is in flight,
    /// they must stay allocated for the rest of the process. RetryNextTurn and Err keep no pointer.
    pub(crate) unsafe fn recv_once(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
        buf: NonNull<u8>,
        len: u32,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        // SAFETY: buf and len are this function's # Safety contract, which keeps them as push
        // asks until the terminal completion; the entry points at no other memory.
        unsafe {
            self.push(
                r,
                &[
                    opcode::Recv::new(types::Fixed(u32::from(slot)), buf.as_ptr(), len)
                        .build()
                        .user_data(encode(Tag::Once, slot, generation)),
                ],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    pub(crate) fn cancel(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
        armed: Armed,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        let tag = match armed {
            Armed::Recv => Tag::Recv,
            Armed::Poll => Tag::Poll,
        };
        // SAFETY: the AsyncCancel entry names its target by user data and points at no memory.
        unsafe {
            self.push(
                r,
                &[opcode::AsyncCancel::new(encode(tag, slot, generation))
                    .build()
                    .user_data(encode(Tag::Cancel, slot, generation))],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    /// # Safety
    /// When the call returns Ok(Submit::Done(())), buf must point to len bytes that stay
    /// allocated, unmoved and unwritten by Rust code until this SQE's terminal completion is
    /// reaped, by reap or by drain; when this Rings is dropped without drain while the SQE is in
    /// flight, they must stay allocated for the rest of the process. RetryNextTurn and Err keep no
    /// pointer.
    pub(crate) unsafe fn send(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
        buf: NonNull<u8>,
        len: u32,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        // SAFETY: buf and len are this function's # Safety contract, which keeps them as push
        // asks until the terminal completion; the entry points at no other memory.
        unsafe {
            self.push(
                r,
                &[
                    opcode::Send::new(types::Fixed(u32::from(slot)), buf.as_ptr(), len)
                        .flags(libc::MSG_NOSIGNAL)
                        .build()
                        .user_data(encode(Tag::Send, slot, generation)),
                ],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    /// Pushes one chain of contiguous SQEs: the SO_LINGER SetSockOpt `linger` names (none for
    /// Keep), the fd-wide cancel when `io` is Pending, then the Close; every SQE but the Close is
    /// hardlinked, so each later SQE runs whatever an earlier one returned.
    pub(crate) fn close(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
        linger: CloseLinger,
        io: CloseIo,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        let fd = types::Fixed(u32::from(slot));
        let [zero, off] = &*self.kernel.lingers;
        let value = match linger {
            CloseLinger::Keep => None,
            // UNSAFE PATH (speed): no orderly close (half-close, then read to the peer's end of stream); server.rs passes Zero only when SIOCOUTQ reported nothing unacknowledged, the kernel sends a reset or a FIN on any close of such a socket, and the contest's hang-up budget was measured with this close
            CloseLinger::Zero => Some(zero),
            CloseLinger::Off => Some(off),
        };
        let set_linger = value.map(|value| {
            opcode::SetSockOpt::new(
                fd,
                u32::try_from(libc::SOL_SOCKET).expect("SOL_SOCKET"),
                u32::try_from(libc::SO_LINGER).expect("SO_LINGER"),
                std::ptr::from_ref(value).cast::<libc::c_void>(),
                u32::try_from(size_of::<libc::linger>()).expect("linger size"),
            )
            .build()
            .flags(squeue::Flags::IO_HARDLINK)
            .user_data(encode(Tag::Linger, slot, generation))
        });
        let cancel = match io {
            CloseIo::Quiet => None,
            CloseIo::Pending => Some(
                opcode::AsyncCancel2::new(types::CancelBuilder::fd(fd).all())
                    .build()
                    .flags(squeue::Flags::IO_HARDLINK)
                    .user_data(encode(Tag::Cancel, slot, generation)),
            ),
        };
        let close = opcode::Close::new(fd)
            .build()
            .user_data(encode(Tag::Close, slot, generation));
        // The SetSockOpt points into the lingers box, which lives as long as the Kernel and so
        // outlasts every SQE; the cancel and the Close name only the fixed slot.
        match (set_linger, cancel) {
            // SAFETY: the lone Close points at no memory.
            (None, None) => unsafe { self.push(r, &[close]) },
            // SAFETY: the SetSockOpt's value is in the lingers box; the Close points at no memory.
            (Some(set_linger), None) => unsafe { self.push(r, &[set_linger, close]) },
            // SAFETY: the cancel and the Close point at no memory.
            (None, Some(cancel)) => unsafe { self.push(r, &[cancel, close]) },
            // SAFETY: the SetSockOpt's value is in the lingers box; the cancel and the Close point
            // at no memory.
            (Some(set_linger), Some(cancel)) => unsafe {
                self.push(r, &[set_linger, cancel, close])
            },
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    /// Asks the socket's unacknowledged send bytes, write_seq - snd_una, through
    /// SOCKET_URING_OP_SIOCOUTQ (1); the completion result carries the byte count.
    pub(crate) fn outq(&mut self, ring: u8, slot: u16, generation: u32) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        // SAFETY: the UringCmd16 entry carries its command inline and points at no memory.
        unsafe {
            self.push(
                r,
                &[opcode::UringCmd16::new(types::Fixed(u32::from(slot)), 1)
                    .build()
                    .user_data(encode(Tag::OutQ, slot, generation))],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    /// Half-closes the write side (SHUT_WR): the peer reads end of stream while this side can
    /// still receive. Compiled in every build; only the safe build's close order calls it.
    pub(crate) fn shutdown(
        &mut self,
        ring: u8,
        slot: u16,
        generation: u32,
    ) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        // SAFETY: the Shutdown entry names a fixed slot and points at no memory.
        unsafe {
            self.push(
                r,
                &[
                    opcode::Shutdown::new(types::Fixed(u32::from(slot)), libc::SHUT_WR)
                        .build()
                        .user_data(encode(Tag::Shutdown, slot, generation)),
                ],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    /// # Panics
    /// Panics for an invalid ring or fixed-slot index.
    pub(crate) fn close_orphan(&mut self, ring: u8, slot: u16) -> io::Result<Submit<()>> {
        let r = self.slot(ring, slot);
        // SAFETY: the Close entry names a fixed slot and points at no memory.
        unsafe {
            self.push(
                r,
                &[opcode::Close::new(types::Fixed(u32::from(slot)))
                    .build()
                    .user_data(encode(Tag::Orphan, slot, 0))],
            )
        }
    }
    /// # Errors
    /// Returns a fatal enter error; retryable pressure returns RetryNextTurn.
    pub(crate) fn fsync(&mut self, fd: BorrowedFd<'_>) -> io::Result<Submit<()>> {
        // SAFETY: the Fsync entry names the descriptor by number and points at no memory.
        unsafe {
            self.push(
                0,
                &[opcode::Fsync::new(types::Fd(fd.as_raw_fd()))
                    .flags(types::FsyncFlags::DATASYNC)
                    .build()
                    .user_data(encode(Tag::Fsync, 0, 0))],
            )
        }
    }
    fn held(&self, r: usize, bid: u16) -> bool {
        bid < self.bufs_per_ring
            && *self
                .kernel
                .rings
                .get(r)
                .expect("r < RINGS from ring_index")
                .held_bits
                .get(usize::from(bid) / 64)
                .expect("usize::from(bid) / 64 < held_bits.len() because bid < bufs_per_ring")
                & (1_u64 << (bid % 64))
                != 0
    }
    /// # Panics
    /// Panics for an invalid ring, a bid not user-held, or len beyond BUF_SIZE.
    pub(crate) fn buffer(&self, ring: u8, bid: u16, len: u32) -> &[u8] {
        let r = self.ring_index(ring);
        assert!(self.held(r, bid), "buffer not held");
        assert!(len <= BUF_SIZE, "buffer length out of range");
        let [memory, _] = self
            .kernel
            .mappings
            .get(r)
            .expect("r < RINGS from ring_index");
        // SAFETY: add needs the offset to stay inside one allocation: bid < bufs_per_ring (held
        // checked it), so bid * BUF_SIZE + BUF_SIZE stays inside this ring's buffer mapping of
        // bufs_per_ring * BUF_SIZE bytes, which lives as long as the Rings.
        let start = unsafe {
            memory
                .address
                .as_ptr()
                .cast::<u8>()
                .add(usize::from(bid) * BUF_SIZE as usize)
        };
        // SAFETY: from_raw_parts needs len initialized bytes that nothing writes during the borrow:
        // the bid is user-held, so the kernel no longer writes it, return_buffer needs &mut self,
        // len <= BUF_SIZE keeps the slice inside that buffer, and the mapping's pages are
        // initialized (zero-filled or written by the kernel).
        unsafe { std::slice::from_raw_parts(start, len as usize) }
    }
    /// # Panics
    /// Panics for an invalid ring or a bid not user-held.
    pub(crate) fn return_buffer(&mut self, ring: u8, bid: u16) {
        let r = self.ring_index(ring);
        assert!(self.held(r, bid), "buffer not held");
        let bufs_per_ring = self.bufs_per_ring;
        let kernel = &mut *self.kernel;
        let state = kernel.rings.get_mut(r).expect("r < RINGS from ring_index");
        let [memory, descriptors] = kernel.mappings.get(r).expect("r < RINGS from ring_index");
        // SAFETY: descriptors and memory are ring r's mappings of bufs_per_ring entries and
        // buffers, a power of two Rings::new validated; the assert proved bid user-held, so
        // bid < bufs_per_ring, and since each bid sits in the ring at most once and this one does
        // not, the entries from the kernel's head to the local tail number at most
        // bufs_per_ring - 1, so the slot at the local tail is not one the kernel can select.
        unsafe { descriptor(descriptors, memory, state.tail, bid, bufs_per_ring) };
        state.tail = state.tail.wrapping_add(1);
        *state
            .held_bits
            .get_mut(usize::from(bid) / 64)
            .expect("the assert proved bid < bufs_per_ring, so its word is inside held_bits") &=
            !(1_u64 << (bid % 64));
        // UNSAFE PATH (speed): unchecked arithmetic; the assert above proves this bid user-held, so held is at least 1
        state.held -= 1;
    }
    /// # Panics
    /// Panics for a ring outside the eight owned rings.
    pub(crate) fn free_buffers(&mut self, ring: u8) -> u32 {
        let r = self.ring_index(ring);
        let bufs_per_ring = self.bufs_per_ring;
        let state = self.state_mut(r);
        if state.io.submission().cq_overflow() {
            return 0;
        }
        let carried = u32::try_from(state.io.completion().len()).expect("bounded CQ");
        u32::from(bufs_per_ring)
            .saturating_sub(state.held)
            .saturating_sub(carried)
    }
    /// Submits ring r's queued SQEs and runs its deferred completions in one enter that does not
    /// wait; a retryable failure leaves both for the next enter.
    fn enter_events(&mut self, r: usize) -> io::Result<()> {
        let pending = self.pending(r);
        match self.enter(r, pending, 0, GETEVENTS, None, false) {
            Ok(_) => Ok(()),
            Err(error) if self.retry(&error) => Ok(()),
            Err(error) => Err(contextual("enter ring", r, error)),
        }
    }
    /// Enters each active ring once, rings 1..7 without waiting and ring 0 last.
    /// k0 is ring 0's share of the request-completion target; the server caps the timeout at 1 ms.
    /// Queued CQEs do not bypass batching.
    /// # Errors
    /// Returns a fatal enter error. Timeouts and retryable errors are WaitEnd values.
    /// # Panics
    /// Panics when k0 is zero.
    pub(crate) fn wait(&mut self, k0: u32, timeout: Option<Duration>) -> io::Result<WaitEnd> {
        assert!(k0 >= 1, "k0 must be at least 1");
        let deadline = timeout.map(|duration| Instant::now() + duration);
        // Returned buffers reach the kernel through each ring's published tail, a plain store, so
        // a ring this wait does not enter publishes here.
        let kernel = &mut *self.kernel;
        for (ring, [_, descriptors]) in kernel.rings.iter_mut().zip(kernel.mappings.iter()) {
            publish(ring, descriptors);
        }
        for r in 1..RINGS {
            if self.state(r).in_flight != 0 {
                self.enter_events(r)?;
            }
        }
        let pending = self.pending(0);
        let remaining = deadline.map(|deadline| deadline.saturating_duration_since(Instant::now()));
        let ts = remaining.map(types::Timespec::from);
        let args = ts.as_ref().map(|ts| types::SubmitArgs::new().timespec(ts));
        if timeout == Some(Duration::ZERO) {
            return match self.enter(0, pending, 0, GETEVENTS, None, true) {
                Ok(n) if n < pending => Ok(WaitEnd::Retry),
                Ok(_) => Ok(WaitEnd::Ready),
                Err(error) if self.retry(&error) => {
                    self.enter_events(0)?;
                    Ok(WaitEnd::Retry)
                }
                Err(error) => Err(contextual("enter ring", 0, error)),
            };
        }
        // One enter submits ring 0's SQEs and then waits; a short submission returns at once, and
        // with SQEs submitted the kernel reports their count rather than ETIME.
        match self.enter(
            0,
            pending,
            k0,
            GETEVENTS | if args.is_some() { EXT_ARG } else { 0 },
            args.as_ref(),
            true,
        ) {
            Ok(n) if n < pending => Ok(WaitEnd::Retry),
            Ok(_) => {
                let ready = self.state_mut(0).io.completion().len();
                if ready >= k0 as usize {
                    Ok(WaitEnd::Count)
                } else if timeout.is_some() {
                    Ok(WaitEnd::Timeout)
                } else {
                    Ok(WaitEnd::Retry)
                }
            }
            Err(error) if error.raw_os_error() == Some(libc::ETIME) => Ok(WaitEnd::Timeout),
            // A retryable failure of the waiting enter still runs ring 0's deferred completions with
            // one enter that does not wait, as rings 1..7 run theirs above; retrying only the waiting
            // form left ring 0's receives unrun while the failure lasted, so the stall sweep closed a
            // connection whose request the kernel held (T-DEFERRED, FIX-853).
            Err(error) if self.retry(&error) => {
                self.enter_events(0)?;
                Ok(WaitEnd::Retry)
            }
            Err(error) => Err(contextual("enter ring", 0, error)),
        }
    }
    /// # Panics
    /// Panics for a zero cap or a corrupt internal completion identity.
    pub(crate) fn reap(&mut self, cap: usize, mut f: impl FnMut(Completion)) -> Reaped {
        assert!(cap >= 1, "cap must be at least 1");
        let ready = self
            .kernel
            .rings
            .each_mut()
            .map(|ring| ring.io.completion().len());
        let overflow = self
            .kernel
            .rings
            .each_mut()
            .map(|ring| ring.io.submission().cq_overflow());
        for present in overflow {
            self.counts.cq_overflow = self.counts.cq_overflow.saturating_add(u64::from(present));
        }
        let taken = allocation(ready, cap, usize::from(self.start));
        let mut count = 0;
        for offset in 0..RINGS {
            let r = (usize::from(self.start) + offset) % RINGS;
            let ring = self
                .kernel
                .rings
                .get_mut(r)
                .expect("r < RINGS from the modulo in the rotated loop");
            let mut cq = ring.io.completion();
            let take = *taken
                .get(r)
                .expect("r < RINGS from the modulo in the rotated loop");
            for _ in 0..take {
                let entry = cq.next().expect("snapshot CQ entries");
                let flags = entry.flags();
                if !cqueue::more(flags) {
                    // UNSAFE PATH (speed): unchecked arithmetic; every terminal completion answers one SQE pushed on this ring, so in_flight is at least 1 here
                    ring.in_flight -= 1;
                }
                if let Some(bid) = cqueue::buffer_select(flags) {
                    assert!(bid < self.bufs_per_ring, "buffer not held");
                    *ring.held_bits.get_mut(usize::from(bid) / 64).expect(
                        "the assert bounds bid below bufs_per_ring, so its word is inside held_bits",
                    ) |= 1_u64 << (bid % 64);
                    // UNSAFE PATH (speed): unchecked arithmetic; held counts user-held bids, at most bufs_per_ring, which Rings::new caps at 32,768
                    ring.held += 1;
                }
                let completion = decode(
                    u8::try_from(r).expect("ring index"),
                    entry.user_data(),
                    entry.result(),
                    flags,
                );
                if let Completion::Accept { result, .. } = completion {
                    assert!(
                        result < 0
                            || u32::try_from(result).expect("accepted slot") < self.slots_per_ring,
                        "accepted slot out of range"
                    );
                }
                f(completion);
                count += 1;
            }
        }
        self.start = (self.start + 1) % 8;
        let left = overflow.into_iter().any(|value| value)
            || self
                .kernel
                .rings
                .iter_mut()
                .any(|ring| !ring.io.completion().is_empty());
        Reaped { count, left }
    }
    /// # Errors
    /// Returns a fatal enter error; retained submissions return RetryNextTurn.
    pub(crate) fn submit_all(&mut self) -> io::Result<Submit<()>> {
        let mut retry = false;
        for r in 0..RINGS {
            let pending = self.pending(r);
            if pending == 0 {
                continue;
            }
            let flags = if self.state_mut(r).io.submission().cq_overflow() {
                GETEVENTS
            } else {
                0
            };
            match self.enter(r, pending, 0, flags, None, false) {
                Ok(n) => retry |= n < pending,
                Err(error) if self.retry(&error) => retry = true,
                Err(error) => return Err(contextual("enter ring", r, error)),
            }
        }
        Ok(if retry {
            Submit::RetryNextTurn
        } else {
            Submit::Done(())
        })
    }
    /// # Panics
    /// Panics for a ring outside the eight owned rings.
    pub(crate) fn active(&self, ring: u8) -> bool {
        self.state(self.ring_index(ring)).in_flight > 0
    }
    pub(crate) fn slots(&self) -> u32 {
        self.slots_per_ring
    }
    pub(crate) fn take_counts(&mut self) -> RingCounts {
        std::mem::take(&mut self.counts)
    }
    pub(crate) fn startup(&self) -> Startup {
        Startup {
            rings: 8,
            slots_per_ring: self.slots_per_ring,
            bufs_per_ring: self.bufs_per_ring,
            pbuf_retry: self.pbuf_retry,
        }
    }
    #[cfg(twelve_fault_inject)]
    fn injected_errno(&self) -> Option<i32> {
        let mut file = File::open(self.enter_path.as_ref()?).ok()?;
        let mut bytes = [0; 17];
        let mut len = 0;
        while len < bytes.len() {
            match file.read(bytes.get_mut(len..).expect("len <= 17")) {
                Ok(0) => break,
                Ok(n) => len += n,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return None,
            }
        }
        if len > 16 {
            return None;
        }
        let text = bytes.get(..len).expect("len <= 17");
        let text = text
            .strip_suffix(b"\r\n")
            .or_else(|| text.strip_suffix(b"\n"))
            .unwrap_or(text);
        match text {
            b"EINTR" => Some(libc::EINTR),
            b"EAGAIN" => Some(libc::EAGAIN),
            b"EBUSY" => Some(libc::EBUSY),
            b"ENOMEM" => Some(libc::ENOMEM),
            _ => None,
        }
    }
    /// Ends every in-flight operation on every ring, for a teardown before any owner of memory
    /// lent to recv_once or send drops; the running server never calls it. No completion reaches
    /// a caller and no carried buffer is returned. A fatal enter error aborts the process, since
    /// memory the kernel may still use is never released.
    #[cfg_attr(
        not(test),
        expect(
            dead_code,
            reason = "only a teardown in the unit tests drains: Server::run never returns"
        )
    )]
    pub(crate) fn drain(&mut self) {
        for r in 0..RINGS {
            if self.state(r).in_flight == 0 {
                continue;
            }
            loop {
                let pending = self.pending(r);
                if pending == 0 {
                    break;
                }
                self.drain_enter(r, pending, 0, 0);
            }
            let cancel = opcode::AsyncCancel2::new(types::CancelBuilder::any())
                .build()
                .user_data(encode(Tag::Cancel, 0, 0));
            let state = self.state_mut(r);
            // SAFETY: push needs the entry's parameters valid for the whole operation: cancel-any
            // names no memory. The loop above emptied the SQ, so the push finds room.
            unsafe { state.io.submission().push(&cancel) }.expect("empty SQ for drain cancel");
            // in_flight counts the SQEs pushed on this ring and not yet answered: the cancel-any is one more.
            state.in_flight = state.in_flight.strict_add(1);
            while self.state(r).in_flight != 0 {
                let pending = self.pending(r);
                self.drain_enter(r, pending, 1, GETEVENTS);
                let state = self.state_mut(r);
                for entry in state.io.completion() {
                    if !cqueue::more(entry.flags()) {
                        // in_flight counts the SQEs pushed on this ring and not yet answered: a terminal completion answers one.
                        state.in_flight = state.in_flight.strict_sub(1);
                    }
                }
            }
        }
    }
    fn drain_enter(&mut self, r: usize, pending: u32, minimum: u32, flags: u32) {
        loop {
            match self.enter(r, pending, minimum, flags, None, false) {
                Ok(_) => return,
                Err(error) if self.retry(&error) => {}
                Err(error) => {
                    // The write's result is ignored: abort follows, and no other report exists.
                    let _ = writeln!(io::stderr().lock(), "io_uring: drain ring {r}: {error}");
                    std::process::abort();
                }
            }
        }
    }
}

impl Drop for Rings {
    fn drop(&mut self) {
        // Leak path: an operation may still use memory (a provided buffer, a caller's lent
        // buffer, a linger value), so nothing the kernel can reach is released and nothing waits.
        if self.kernel.rings.iter().any(|ring| ring.in_flight != 0) {
            return;
        }
        // Release path. A failed unregister leaves a ring alive until the thread exits but
        // touches no memory, every operation having ended, so both results are ignored.
        for ring in &self.kernel.rings {
            unregister_pbuf(ring.io.as_fd());
            unregister_index(ring.io.as_fd(), ring.index);
        }
        // SAFETY: ManuallyDrop::drop needs a value never used again: this runs once, in Drop, and
        // no later code reads self.kernel.
        unsafe { ManuallyDrop::drop(&mut self.kernel) };
    }
}

// An io_uring failure with its step and ring; the io::Error that caused it stays its source.
#[derive(Debug)]
struct RingError {
    action: &'static str,
    ring: usize,
    source: io::Error,
}
impl std::fmt::Display for RingError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "io_uring: {} {}: {}",
            self.action, self.ring, self.source
        )
    }
}
impl std::error::Error for RingError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.source)
    }
}

fn contextual(action: &'static str, ring: usize, error: io::Error) -> io::Error {
    io::Error::new(
        error.kind(),
        RingError {
            action,
            ring,
            source: error,
        },
    )
}
fn missing(name: &str) -> io::Error {
    io::Error::new(
        io::ErrorKind::Unsupported,
        format!("io_uring: missing {name}"),
    )
}

/// Makes one io_uring_register call for a ring.
///
/// # Safety
/// - `fd` is this thread's live ring descriptor: the IoUring that owns it is not dropped.
/// - `arg` points to an owned value with the kernel's layout for `opcode` and `entries`
///   (io_uring_rsrc_update for the RING_FDS opcodes, io_uring_buf_reg for the PBUF_RING
///   opcodes), derived with `&raw mut` and writable, since IORING_REGISTER_RING_FDS fills its
///   offset.
/// - Memory a registration names (the descriptor ring of IORING_REGISTER_PBUF_RING) stays mapped
///   until it is unregistered.
unsafe fn register(fd: RawFd, opcode: u32, arg: *mut libc::c_void, entries: u32) -> io::Result<()> {
    // SAFETY: io_uring_register reads, and for RING_FDS writes, `entries` values of the layout
    // `opcode` names at `arg` and uses `fd` only for this call; the caller guarantees both.
    let result = unsafe { libc::syscall(libc::SYS_io_uring_register, fd, opcode, arg, entries) };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
fn unregister_pbuf(fd: BorrowedFd<'_>) {
    let mut reg = BufReg {
        ring_addr: 0,
        ring_entries: 0,
        bgid: 0,
        flags: 0,
        resv: [0; 3],
    };
    // SAFETY: fd is a live ring descriptor borrowed for this call; reg is an owned local with
    // io_uring_buf_reg's asserted layout, passed through &raw mut; IORING_UNREGISTER_PBUF_RING
    // reads only its group id, 0, and names no memory. A failed unregister touches no memory, so
    // its result is ignored.
    let _ = unsafe { register(fd.as_raw_fd(), 23, (&raw mut reg).cast(), 1) };
}
fn unregister_index(fd: BorrowedFd<'_>, index: u32) {
    let mut update = ResourceUpdate {
        offset: index,
        resv: 0,
        data: 0,
    };
    // SAFETY: fd is a live ring descriptor borrowed for this call; update is an owned local with
    // io_uring_rsrc_update's asserted layout, passed through &raw mut, naming the registered index
    // this thread created; it names no memory. A failed unregister touches no memory, so its result
    // is ignored.
    let _ = unsafe { register(fd.as_raw_fd(), 21, (&raw mut update).cast(), 1) };
}
fn register_pbuf(
    reg: &mut BufReg,
    mut call: impl FnMut(&BufReg) -> io::Result<()>,
) -> io::Result<bool> {
    match call(reg) {
        Ok(()) => Ok(false),
        Err(error) if error.raw_os_error() == Some(libc::EINVAL) => {
            reg.resv[0] = 1;
            call(reg)?;
            Ok(true)
        }
        Err(error) => Err(error),
    }
}
/// Writes the descriptor of buffer `bid` into the provided-buffer ring slot at `tail`.
///
/// # Safety
/// `descriptors` and `memory` must be one ring's descriptor mapping of `count` 16-byte entries
/// and buffer mapping of `count` * BUF_SIZE bytes; `count` must be a power of two of at least 1
/// and `bid` below it; the slot at `tail & (count - 1)` must be one the kernel cannot select,
/// owned by Rust code until publish stores a tail past it.
unsafe fn descriptor(descriptors: &Mapping, memory: &Mapping, tail: u16, bid: u16, count: u16) {
    // UNSAFE PATH (speed): unchecked arithmetic; Rings::new accepts only a power of two of at least 1 for bufs_per_ring, so count - 1 cannot underflow
    let index = usize::from(tail & (count - 1));
    // SAFETY: add needs the offset to stay inside one allocation: index < count, and this
    // function's contract gives a descriptor mapping of count 16-byte entries from its
    // page-aligned base.
    let slot = unsafe {
        descriptors
            .address
            .as_ptr()
            .cast::<types::BufRingEntry>()
            .add(index)
    };
    // SAFETY: the reference needs an aligned, initialized slot no one else accesses: the slot is
    // 16-byte aligned in the page-aligned anonymous mapping, this function's contract keeps the
    // kernel off it until publish stores the tail, and the three setters below never write entry
    // zero's tail word.
    let entry = unsafe { &mut *slot };
    // SAFETY: add needs the offset to stay inside one allocation: this function's contract gives
    // bid < count, so bid * BUF_SIZE stays inside the buffer mapping of count * BUF_SIZE bytes.
    let address = unsafe {
        memory
            .address
            .as_ptr()
            .cast::<u8>()
            .add(usize::from(bid) * BUF_SIZE as usize)
    };
    entry.set_addr(address.addr() as u64);
    entry.set_len(BUF_SIZE);
    entry.set_bid(bid);
}
fn publish(ring: &mut Ring, descriptors: &Mapping) {
    if ring.tail == ring.published {
        return;
    }
    // SAFETY: BufRingEntry::tail needs the address of the ring's first, initialized entry: the
    // descriptor mapping's page-aligned base is entry zero, written before the first publish.
    let tail = unsafe { types::BufRingEntry::tail(descriptors.address.as_ptr().cast()) };
    // SAFETY: from_ptr needs a u16-aligned pointer, valid while the atomic is used and accessed
    // only atomically meanwhile: the tail word sits at byte 14 of the page-aligned entry zero, the
    // mapping outlives this call, and Rust reaches it only here.
    let tail = unsafe { AtomicU16::from_ptr(tail.cast_mut()) };
    // Pairs with the kernel's acquire load of the tail: it sees every descriptor stored before.
    tail.store(ring.tail, Ordering::Release);
    ring.published = ring.tail;
}
// The op tag in bits 0 to 7 of every SQE's user data.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
enum Tag {
    Accept = 1,
    Recv = 2,
    Once = 3,
    Poll = 4,
    Send = 5,
    Cancel = 6,
    AcceptCancel = 7,
    Close = 8,
    Orphan = 9,
    Fsync = 10,
    OutQ = 11,
    Linger = 12,
    Shutdown = 13,
}
impl From<Tag> for u64 {
    fn from(tag: Tag) -> Self {
        // The discriminants are the tag values 1 to 13.
        u64::from(tag as u8)
    }
}
impl TryFrom<u64> for Tag {
    type Error = u64;

    fn try_from(value: u64) -> Result<Self, u64> {
        match value {
            1 => Ok(Tag::Accept),
            2 => Ok(Tag::Recv),
            3 => Ok(Tag::Once),
            4 => Ok(Tag::Poll),
            5 => Ok(Tag::Send),
            6 => Ok(Tag::Cancel),
            7 => Ok(Tag::AcceptCancel),
            8 => Ok(Tag::Close),
            9 => Ok(Tag::Orphan),
            10 => Ok(Tag::Fsync),
            11 => Ok(Tag::OutQ),
            12 => Ok(Tag::Linger),
            13 => Ok(Tag::Shutdown),
            other => Err(other),
        }
    }
}
fn encode(tag: Tag, slot: u16, generation: u32) -> u64 {
    u64::from(tag) | (u64::from(slot) << 8) | (u64::from(generation) << 32)
}
fn decode(ring: u8, data: u64, result: i32, flags: u32) -> Completion {
    let slot = u16::try_from((data >> 8) & 65535).expect("slot bits");
    let generation = u32::try_from(data >> 32).expect("generation bits");
    let more = cqueue::more(flags);
    match Tag::try_from(data & 255).expect("corrupt completion tag") {
        Tag::Accept => Completion::Accept {
            ring,
            arm: generation,
            result,
            more,
        },
        Tag::Recv => Completion::Recv {
            ring,
            slot,
            generation,
            result,
            buffer: cqueue::buffer_select(flags),
            more,
        },
        Tag::Once => Completion::RecvOnce {
            ring,
            slot,
            generation,
            result,
        },
        Tag::Poll => Completion::Poll {
            ring,
            slot,
            generation,
            result,
            more,
        },
        Tag::Send => Completion::Send {
            ring,
            slot,
            generation,
            result,
        },
        Tag::Cancel => Completion::Cancel {
            ring,
            slot,
            generation,
            result,
        },
        Tag::AcceptCancel => Completion::AcceptCancel {
            ring,
            arm: generation,
            result,
        },
        Tag::Close => Completion::Close {
            ring,
            slot,
            generation,
            result,
        },
        Tag::Orphan => Completion::OrphanClose { ring, slot, result },
        Tag::Fsync => Completion::Fsync { result },
        Tag::OutQ => Completion::OutQ {
            ring,
            slot,
            generation,
            result,
        },
        Tag::Linger => Completion::Linger {
            ring,
            slot,
            generation,
            result,
        },
        Tag::Shutdown => Completion::Shutdown {
            ring,
            slot,
            generation,
            result,
        },
    }
}
fn allocation(ready: [usize; RINGS], cap: usize, start: usize) -> [usize; RINGS] {
    if ready.iter().sum::<usize>() <= cap {
        return ready;
    }
    let mut low = 0;
    let mut high = *ready.iter().max().expect("eight rings");
    while low + 1 < high {
        let middle = low + (high - low) / 2;
        if ready.iter().map(|&n| n.min(middle)).sum::<usize>() <= cap {
            low = middle;
        } else {
            high = middle;
        }
    }
    let mut taken = ready.map(|n| n.min(low));
    let mut remaining = cap - taken.iter().sum::<usize>();
    for offset in 0..RINGS {
        let r = (start + offset) % RINGS;
        let more = *ready.get(r).expect("r < RINGS from the modulo") > low;
        if remaining != 0 && more {
            *taken.get_mut(r).expect("r < RINGS from the modulo") += 1;
            remaining -= 1;
        }
    }
    taken
}
