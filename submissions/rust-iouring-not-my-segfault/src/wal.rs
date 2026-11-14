use crate::stats::stderr_line;
use std::ffi::{CString, c_int};
use std::os::fd::{BorrowedFd, RawFd};
use std::ptr::{self, NonNull};
use std::time::{Duration, Instant};

use sys::{
    Checkpointer, TWELVE_CKPT_DONE, TWELVE_CKPT_WAIT_SLICE, TWELVE_CKPT_WAIT_SYNC,
    twelve_ckpt_close, twelve_ckpt_open, twelve_ckpt_restart, twelve_ckpt_result,
    twelve_ckpt_resume, twelve_ckpt_start, twelve_ckpt_sync_fd,
};

// The hand-written mirror of checkpointer.c's public ABI, which has no header; only Wal's
// methods use these items. bindgen is outside the pinned toolchain, so each function is called
// once by a native test instead of a regeneration diff.
mod sys {
    use std::ffi::{c_char, c_int};

    pub(super) const TWELVE_CKPT_DONE: c_int = 0;
    pub(super) const TWELVE_CKPT_WAIT_SYNC: c_int = 1;
    pub(super) const TWELVE_CKPT_WAIT_SLICE: c_int = 2;

    // The opaque struct twelve_ckpt.
    #[repr(C)]
    pub(super) struct Checkpointer {
        _opaque: [u8; 0],
    }

    unsafe extern "C" {
        pub(super) fn twelve_ckpt_open(path: *const c_char, out: *mut *mut Checkpointer) -> c_int;
        pub(super) fn twelve_ckpt_start(checkpointer: *mut Checkpointer) -> c_int;
        pub(super) fn twelve_ckpt_restart(checkpointer: *mut Checkpointer) -> c_int;
        pub(super) fn twelve_ckpt_resume(checkpointer: *mut Checkpointer, result: c_int) -> c_int;
        pub(super) fn twelve_ckpt_sync_fd(checkpointer: *const Checkpointer) -> c_int;
        pub(super) fn twelve_ckpt_result(
            checkpointer: *const Checkpointer,
            result: *mut c_int,
            log: *mut c_int,
            checkpointed: *mut c_int,
        );
        pub(super) fn twelve_ckpt_close(checkpointer: *mut Checkpointer) -> c_int;
    }
}

// The state twelve_ckpt_start, twelve_ckpt_restart and twelve_ckpt_resume return.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum CkptState {
    Done,
    WaitSync,
    WaitSlice,
}

impl TryFrom<c_int> for CkptState {
    type Error = c_int;

    fn try_from(value: c_int) -> Result<Self, c_int> {
        match value {
            TWELVE_CKPT_DONE => Ok(Self::Done),
            TWELVE_CKPT_WAIT_SYNC => Ok(Self::WaitSync),
            TWELVE_CKPT_WAIT_SLICE => Ok(Self::WaitSlice),
            unknown => Err(unknown),
        }
    }
}

pub(crate) struct WalSettings {
    pub(crate) gate_frames: u32,
    pub(crate) ckpt_restart: bool,
}

#[derive(Debug, Eq, PartialEq)]
pub(crate) enum WalStep {
    Idle,
    Fsync,
    Slice,
}

#[derive(Default)]
pub(crate) struct WalCounts {
    pub(crate) passes: u64,
    pub(crate) pass_busy: u64,
    pub(crate) pass_errors: u64,
    pub(crate) slices: u64,
    pub(crate) fsyncs: u64,
    pub(crate) fsync_us_max: u64,
    pub(crate) gate_holds: u64,
    pub(crate) gate_hold_us_max: u64,
    pub(crate) wal_restarts: u64,
    pub(crate) wal_frames_max: u64,
    pub(crate) commit_frames_max: u64,
    pub(crate) header_sync_us_max: u64,
    pub(crate) restart_writes: u64,
}

#[derive(Clone, Copy)]
enum Gate {
    Open,
    Holding { since: Instant },
    Restarting { since: Instant },
}

#[derive(Clone, Copy)]
enum RunKind {
    Pass { began_held: bool },
    Restart { saw_wal_sync: bool },
}

// AwaitSubmit and InFlight keep the checkpointer's descriptor as a private number, which only
// sync_fd (lending it as a BorrowedFd) and synced (its nonnegative check) read; it never
// leaves Wal as a RawFd.
#[derive(Clone, Copy)]
enum Fiber {
    Idle,
    AwaitSubmit {
        kind: RunKind,
        fd: RawFd,
    },
    InFlight {
        kind: RunKind,
        fd: RawFd,
        submitted: Instant,
    },
    Completed {
        kind: RunKind,
        result: i32,
        submitted: Instant,
        completed: Instant,
    },
    Slice {
        kind: RunKind,
    },
    #[cfg(twelve_fault_inject)]
    DelayedCompletion {
        kind: RunKind,
        result: i32,
        submitted: Instant,
        phase: fault::Phase,
    },
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum PassRequest {
    None,
    Ready,
    RetryOnTurn,
}

// Why writes are held: at the gate, until a pass has copied the whole WAL, or for the tail pass that copies
// what was committed beside a bulk pass, HOLD_MAX at most.
#[derive(Clone, Copy)]
enum HoldKind {
    Gate,
    Tail,
}

// A pass starts at every PASS_FRAMES_MAX new frames at most, 16 MiB of 4 KiB pages, whatever the gate.
const PASS_FRAMES_MAX: u32 = 4_096;
// The longest a tail hold keeps writes waiting: half the contest's p95 bar of 500 ms, so a write that meets a
// capped hold and the commits queued behind it still answers inside that bar.
const HOLD_MAX: Duration = Duration::from_millis(250);

pub(crate) struct Wal {
    checkpointer: NonNull<Checkpointer>,
    gate_frames: u32,
    half: u64,
    ckpt_restart: bool,
    seeded: bool,
    last_frames: u32,
    next_threshold: u64,
    pass_request: PassRequest,
    restart_ready: bool,
    gate: Gate,
    // When the current tail hold ends at the latest; None outside a hold, during a hold at the gate and
    // during a restart.
    hold_deadline: Option<Instant>,
    // The cap ended the hold the running pass began under, so commits ran beside that pass.
    hold_broken: bool,
    fiber: Fiber,
    after_time: Option<Instant>,
    held_time: Option<Instant>,
    submit_time: Option<Instant>,
    counts_time: Option<Instant>,
    counts: WalCounts,
    #[cfg(twelve_fault_inject)]
    fault: fault::Faults,
}

fn invariant(kind: &str) -> ! {
    // One write, whole or not at all (stderr_line); the process ends with status 1 either way.
    let _ = stderr_line(|out| writeln!(out, "wal invariant: {kind}"));
    std::process::exit(1)
}

fn elapsed(start: Instant, end: Instant) -> u64 {
    u64::try_from(end.saturating_duration_since(start).as_nanos() / 1000).unwrap_or(u64::MAX)
}

fn observe(previous: &mut Option<Instant>, now: Instant) {
    if previous.is_some_and(|previous| now < previous) {
        invariant("clock");
    }
    *previous = Some(now);
}

impl Wal {
    pub(crate) fn open(path: &str, settings: &WalSettings) -> Result<Self, String> {
        let error = |stage: &str, code: c_int| format!("wal open: {stage} (sqlite {})", code & 255);
        // The checkpoint and fault-path stages name the path they failed on.
        let path_error = |stage: &str, code: c_int| {
            format!(
                "wal open: {stage} {} (sqlite {})",
                path.escape_debug(),
                code & 255
            )
        };
        if path.is_empty() {
            return Err(error("path", 0));
        }
        let path_c = CString::new(path).map_err(|_| error("path", 0))?;
        let valid = if cfg!(twelve_small_limits) {
            settings.gate_frames == 64
        } else {
            (1024..=65_536).contains(&settings.gate_frames)
        };
        if !valid {
            return Err(error("settings", 0));
        }
        #[cfg(twelve_fault_inject)]
        let fault = fault::Faults::new(path).map_err(|_| path_error("fault-path", 0))?;
        let mut checkpointer = ptr::null_mut();
        // SAFETY: the C string and writable output remain live for this call;
        // the accepted C open unwinds partial ownership and returns NULL on error.
        let code = unsafe { twelve_ckpt_open(path_c.as_ptr(), &mut checkpointer) };
        if code != 0 {
            return Err(path_error("checkpoint", code));
        }
        let checkpointer =
            NonNull::new(checkpointer).unwrap_or_else(|| invariant("checkpoint-state"));
        let half = u64::from((settings.gate_frames / 2).min(PASS_FRAMES_MAX));
        Ok(Self {
            checkpointer,
            gate_frames: settings.gate_frames,
            half,
            ckpt_restart: settings.ckpt_restart,
            seeded: false,
            last_frames: 0,
            next_threshold: half,
            pass_request: PassRequest::None,
            restart_ready: false,
            gate: Gate::Open,
            hold_deadline: None,
            hold_broken: false,
            fiber: Fiber::Idle,
            after_time: None,
            held_time: None,
            submit_time: None,
            counts_time: None,
            counts: WalCounts::default(),
            #[cfg(twelve_fault_inject)]
            fault,
        })
    }

    fn require_seed(&self) {
        if !self.seeded {
            invariant("seed");
        }
    }

    pub(crate) fn seed_frames(&mut self, frames: u32, now: Instant) {
        if self.seeded {
            invariant("seed");
        }
        self.seeded = true;
        self.last_frames = frames;
        self.counts.wal_frames_max = u64::from(frames);
        if u64::from(frames) >= self.half {
            self.pass_request = PassRequest::Ready;
        }
        if frames >= self.gate_frames {
            self.hold(now, HoldKind::Gate);
        }
    }

    fn hold(&mut self, now: Instant, kind: HoldKind) {
        if matches!(self.gate, Gate::Open) {
            self.gate = Gate::Holding { since: now };
            self.hold_deadline = match kind {
                HoldKind::Gate => None,
                HoldKind::Tail => Some(now + HOLD_MAX),
            };
            self.counts.gate_holds = self.counts.gate_holds.saturating_add(1);
            self.pass_request = PassRequest::Ready;
        }
    }

    pub(crate) fn after_commit(&mut self, frames: u32, now: Instant) {
        self.require_seed();
        observe(&mut self.after_time, now);
        if !matches!(self.gate, Gate::Open) {
            invariant("held-write");
        }
        let restarted = self.restart_ready || frames < self.last_frames;
        let produced = if restarted {
            self.counts.wal_restarts = self.counts.wal_restarts.saturating_add(1);
            self.counts.header_sync_us_max = self
                .counts
                .header_sync_us_max
                .max(elapsed(now, Instant::now()));
            self.next_threshold = self.half;
            self.restart_ready = false;
            frames
        } else {
            frames
                .checked_sub(self.last_frames)
                .unwrap_or_else(|| invariant("checkpoint-state"))
        };
        self.last_frames = frames;
        self.counts.commit_frames_max = self.counts.commit_frames_max.max(u64::from(produced));
        self.counts.wal_frames_max = self.counts.wal_frames_max.max(u64::from(frames));
        if u64::from(frames) >= self.next_threshold {
            self.pass_request = PassRequest::Ready;
            self.next_threshold = u64::from(frames)
                .checked_add(self.half)
                .unwrap_or_else(|| invariant("checkpoint-state"));
        }
        if frames >= self.gate_frames {
            self.hold(now, HoldKind::Gate);
        }
    }

    fn sample_hold(&mut self, now: Instant) {
        match self.gate {
            Gate::Open => {}
            Gate::Holding { since } | Gate::Restarting { since } => {
                self.counts.gate_hold_us_max =
                    self.counts.gate_hold_us_max.max(elapsed(since, now));
            }
        }
    }

    pub(crate) fn writes_held(&mut self, now: Instant) -> bool {
        self.require_seed();
        observe(&mut self.held_time, now);
        self.sample_hold(now);
        if matches!(self.gate, Gate::Holding { .. })
            && self.hold_deadline.is_some_and(|deadline| now >= deadline)
        {
            // The tail pass outlasted its hold: writes go on, so that pass ends uncovered and an unheld
            // pass follows it.
            self.hold_broken = true;
            self.release();
        }
        !matches!(self.gate, Gate::Open)
    }

    // When the loop must look at the gate again: the end of a capped hold.
    pub(crate) fn hold_deadline(&self) -> Option<Instant> {
        self.hold_deadline
    }

    pub(crate) fn runnable(&self) -> bool {
        self.require_seed();
        match self.fiber {
            Fiber::Slice { .. } | Fiber::Completed { .. } => true,
            Fiber::Idle => {
                self.pass_request == PassRequest::Ready
                    || matches!(self.gate, Gate::Restarting { .. })
            }
            Fiber::AwaitSubmit { .. } | Fiber::InFlight { .. } => false,
            #[cfg(twelve_fault_inject)]
            Fiber::DelayedCompletion { .. } => false,
        }
    }

    pub(crate) fn step(&mut self) -> WalStep {
        self.require_seed();
        let (kind, state) = match self.fiber {
            Fiber::Idle => {
                if matches!(self.gate, Gate::Restarting { .. }) {
                    let kind = RunKind::Restart {
                        saw_wal_sync: false,
                    };
                    // SAFETY: this is the sole owner; the previous run reached
                    // DONE and no Store transaction is active at the call boundary.
                    (kind, unsafe {
                        twelve_ckpt_restart(self.checkpointer.as_ptr())
                    })
                } else if self.pass_request != PassRequest::None {
                    self.pass_request = PassRequest::None;
                    self.hold_broken = false;
                    self.counts.passes = self.counts.passes.saturating_add(1);
                    let kind = RunKind::Pass {
                        began_held: !matches!(self.gate, Gate::Open),
                    };
                    // SAFETY: Idle has no suspended C run or borrowed kernel fd.
                    (kind, unsafe {
                        twelve_ckpt_start(self.checkpointer.as_ptr())
                    })
                } else {
                    return WalStep::Idle;
                }
            }
            Fiber::Slice { kind } => {
                // SAFETY: this run yielded a page slice on an earlier call; no
                // other C operation or sync is active and slice resumes with 0.
                (kind, unsafe {
                    twelve_ckpt_resume(self.checkpointer.as_ptr(), 0)
                })
            }
            Fiber::Completed { kind, result, .. } => {
                // SAFETY: the one submitted sync has its recorded real result;
                // the suspended C run and borrowed fd remain alive until resume.
                (kind, unsafe {
                    twelve_ckpt_resume(self.checkpointer.as_ptr(), result)
                })
            }
            #[cfg(twelve_fault_inject)]
            Fiber::DelayedCompletion {
                kind,
                result,
                submitted,
                phase,
            } => {
                if self.fault.delayed(phase) {
                    return WalStep::Idle;
                }
                let completed = Instant::now();
                self.counts.fsync_us_max =
                    self.counts.fsync_us_max.max(elapsed(submitted, completed));
                // SAFETY: the kernel sync completed before it was delayed. Only
                // the test result handoff waited; this is its single C resume.
                (kind, unsafe {
                    twelve_ckpt_resume(self.checkpointer.as_ptr(), result)
                })
            }
            Fiber::AwaitSubmit { .. } | Fiber::InFlight { .. } => return WalStep::Idle,
        };
        self.fiber = Fiber::Idle;
        let Ok(state) = CkptState::try_from(state) else {
            invariant("checkpoint-state");
        };
        match state {
            CkptState::Done => {
                self.finished(kind);
                WalStep::Idle
            }
            CkptState::WaitSync => {
                // SAFETY: WAIT_SYNC exposes this run's borrowed ordinary fd.
                let fd = unsafe { twelve_ckpt_sync_fd(self.checkpointer.as_ptr()) };
                if fd < 0 {
                    invariant("sync-fd");
                }
                let kind = match kind {
                    RunKind::Pass { .. } => kind,
                    RunKind::Restart { .. } => RunKind::Restart { saw_wal_sync: true },
                };
                #[cfg(twelve_fault_inject)]
                {
                    // SAFETY: fd is nonnegative (checked above), so it is not -1, and the
                    // checkpointer keeps it open from WAIT_SYNC until the resume after its
                    // completion, which outlasts this classification.
                    let borrowed = unsafe { BorrowedFd::borrow_raw(fd) };
                    self.fault.classify(borrowed, kind);
                }
                self.fiber = Fiber::AwaitSubmit { kind, fd };
                WalStep::Fsync
            }
            CkptState::WaitSlice => {
                self.counts.slices = self.counts.slices.saturating_add(1);
                self.fiber = Fiber::Slice { kind };
                WalStep::Slice
            }
        }
    }

    pub(crate) fn sync_fd(&self) -> BorrowedFd<'_> {
        self.require_seed();
        let (Fiber::AwaitSubmit { fd, .. } | Fiber::InFlight { fd, .. }) = self.fiber else {
            invariant("sync-state");
        };
        // SAFETY: borrow_raw needs a number other than -1 and a descriptor that stays open while
        // the borrow lives. step validated fd nonnegative at WAIT_SYNC, and the checkpointer keeps
        // it open from WAIT_SYNC until the resume after its completion; that resume, like Drop,
        // needs &mut self, which cannot be taken while this borrow of self lives.
        unsafe { BorrowedFd::borrow_raw(fd) }
    }

    pub(crate) fn sync_submitted(&mut self, now: Instant) {
        self.require_seed();
        observe(&mut self.submit_time, now);
        let Fiber::AwaitSubmit { kind, fd } = self.fiber else {
            invariant("sync-state");
        };
        self.fiber = Fiber::InFlight {
            kind,
            fd,
            submitted: now,
        };
        self.counts.fsyncs = self.counts.fsyncs.saturating_add(1);
    }

    pub(crate) fn synced(&mut self, result: i32) {
        self.require_seed();
        if result > 0 || result == i32::MIN {
            invariant("sync-result");
        }
        let Fiber::InFlight {
            kind,
            fd,
            submitted,
        } = self.fiber
        else {
            invariant("sync-state");
        };
        if fd < 0 {
            invariant("sync-fd");
        }
        let completed = Instant::now();
        #[cfg(twelve_fault_inject)]
        let result = {
            let phase = self.fault.phase();
            if result == 0 && self.fault.failed(phase) {
                -libc::EIO
            } else if result == 0 && self.fault.delayed(phase) {
                self.fiber = Fiber::DelayedCompletion {
                    kind,
                    result,
                    submitted,
                    phase,
                };
                return;
            } else {
                result
            }
        };
        let state = Fiber::Completed {
            kind,
            result,
            submitted,
            completed,
        };
        if let Fiber::Completed {
            submitted,
            completed,
            ..
        } = state
        {
            self.counts.fsync_us_max = self.counts.fsync_us_max.max(elapsed(submitted, completed));
        }
        self.fiber = state;
    }

    fn release(&mut self) {
        self.sample_hold(Instant::now());
        self.gate = Gate::Open;
        self.hold_deadline = None;
    }

    fn finished(&mut self, kind: RunKind) {
        let (mut result, mut log, mut checkpointed) = (0, -1, -1);
        // SAFETY: DONE was just returned; these writable scalar outputs match the
        // pinned C ABI and are consumed before any later start/resume.
        unsafe {
            twelve_ckpt_result(
                self.checkpointer.as_ptr(),
                &mut result,
                &mut log,
                &mut checkpointed,
            )
        };
        if result < 0 {
            invariant("checkpoint-state");
        }
        let code = result & 255;
        match kind {
            RunKind::Pass { began_held } => {
                if code == 5 {
                    self.counts.pass_busy = self.counts.pass_busy.saturating_add(1);
                } else if code != 0 {
                    self.counts.pass_errors = self.counts.pass_errors.saturating_add(1);
                }
                let full = code == 0 && log >= 0 && checkpointed == log;
                let held_through = began_held && !self.hold_broken;
                let covers = full
                    && (held_through
                        || u32::try_from(log).is_ok_and(|log| log >= self.last_frames));
                if covers {
                    if held_through {
                        self.last_frames =
                            u32::try_from(log).unwrap_or_else(|_| invariant("checkpoint-state"));
                    }
                    self.restart_ready = true;
                    self.pass_request = PassRequest::None;
                    match self.gate {
                        Gate::Open => {}
                        Gate::Holding { since } => {
                            if self.ckpt_restart {
                                // The restart's own hold has no cap: its header write runs as a
                                // writer, so no commit may run beside it.
                                self.gate = Gate::Restarting { since };
                                self.hold_deadline = None;
                            } else {
                                self.release();
                            }
                        }
                        Gate::Restarting { .. } => invariant("checkpoint-state"),
                    }
                } else if code != 0 || !matches!(self.gate, Gate::Open) {
                    self.pass_request = PassRequest::RetryOnTurn;
                } else if full && began_held {
                    // The cap ended this pass's hold and commits ran beside it: copy unheld next; the tail
                    // hold follows that pass.
                    self.pass_request = PassRequest::Ready;
                } else if full {
                    // A bulk pass, complete, with commits beside it: hold writes for the tail pass, which
                    // copies only those frames. SQLite syncs the database only in a pass that reaches
                    // the WAL's end, so the tail pass's database sync is the cycle's one.
                    self.hold(Instant::now(), HoldKind::Tail);
                }
            }
            RunKind::Restart { saw_wal_sync } => {
                let Gate::Restarting { since } = self.gate else {
                    invariant("checkpoint-state");
                };
                if code == 0 {
                    self.counts.restart_writes = self.counts.restart_writes.saturating_add(1);
                    self.counts.commit_frames_max = self.counts.commit_frames_max.max(1);
                    if saw_wal_sync {
                        self.counts.wal_restarts = self.counts.wal_restarts.saturating_add(1);
                        self.last_frames = 1;
                        self.counts.wal_frames_max = self.counts.wal_frames_max.max(1);
                        self.next_threshold = self.half;
                        self.restart_ready = false;
                        self.pass_request = PassRequest::None;
                        self.release();
                        return;
                    }
                    self.last_frames = self
                        .last_frames
                        .checked_add(1)
                        .unwrap_or_else(|| invariant("checkpoint-state"));
                    self.counts.wal_frames_max =
                        self.counts.wal_frames_max.max(u64::from(self.last_frames));
                } else {
                    // The line is diagnostic only, one write, whole or not at all (stderr_line): a
                    // dropped or failed line leaves the hold, the fresh pass request and every
                    // counter as they are.
                    let _ = stderr_line(|out| writeln!(out, "wal restart failed: sqlite {code}"));
                }
                self.restart_ready = false;
                self.gate = Gate::Holding { since };
                self.pass_request = PassRequest::RetryOnTurn;
            }
        }
    }

    pub(crate) fn take_counts(&mut self, now: Instant) -> WalCounts {
        self.require_seed();
        observe(&mut self.counts_time, now);
        self.sample_hold(now);
        std::mem::take(&mut self.counts)
    }
}

impl Drop for Wal {
    fn drop(&mut self) {
        if matches!(self.fiber, Fiber::InFlight { .. }) {
            invariant("sync-state");
        }
        // SAFETY: no kernel operation can still use the borrowed fd. C owns and
        // settles any remaining slice/unsubmitted sync, then frees its connection
        // and guarded stack. Store remains alive until after this owner drops.
        let code = unsafe { twelve_ckpt_close(self.checkpointer.as_ptr()) };
        if code != 0 {
            // One write, whole or not at all (stderr_line); Drop cannot report a failed write of
            // this diagnostic, so it is ignored.
            let _ = stderr_line(|out| writeln!(out, "wal close: sqlite {}", code & 255));
        }
    }
}

#[cfg(twelve_fault_inject)]
mod fault {
    use super::{RunKind, invariant};
    use std::ffi::{CStr, CString};
    use std::mem::MaybeUninit;
    use std::os::fd::{AsRawFd, BorrowedFd};
    use std::os::unix::ffi::OsStringExt;

    #[derive(Clone, Copy)]
    pub(super) enum Phase {
        Wal,
        Database,
        Restart,
    }

    pub(super) struct Faults {
        paths: [CString; 6],
        database: Vec<u8>,
        link: Vec<u8>,
        phase: Option<Phase>,
    }

    fn present(path: &CStr) -> bool {
        let mut metadata = MaybeUninit::<libc::stat>::uninit();
        // SAFETY: stat follows the precomputed terminated path; its valid output
        // storage is never read. Only presence, not file contents, matters.
        unsafe { libc::stat(path.as_ptr(), metadata.as_mut_ptr()) == 0 }
    }

    impl Faults {
        pub(super) fn new(path: &str) -> Result<Self, ()> {
            let database = std::fs::canonicalize(path)
                .map_err(|_| ())?
                .into_os_string()
                .into_vec();
            let suffixes = [
                "-fault-wal-wal-sync-fail",
                "-fault-wal-wal-sync-delay",
                "-fault-wal-db-sync-fail",
                "-fault-wal-db-sync-delay",
                "-fault-wal-restart-sync-fail",
                "-fault-wal-restart-sync-delay",
            ];
            let paths = suffixes.map(|suffix| {
                CString::new(format!("{path}{suffix}"))
                    .expect("validated path and suffix contain no NUL")
            });
            let mut link = Vec::new();
            link.try_reserve_exact(database.len().checked_add(5).ok_or(())?)
                .map_err(|_| ())?;
            link.resize(database.len() + 5, 0);
            Ok(Self {
                paths,
                database,
                link,
                phase: None,
            })
        }

        pub(super) fn classify(&mut self, fd: BorrowedFd<'_>, kind: RunKind) {
            let mut path = [0_u8; b"/proc/self/fd/".len() + 10 + 1];
            let prefix = b"/proc/self/fd/";
            path.get_mut(..prefix.len())
                .expect("the 25-byte buffer holds the 14-byte prefix")
                .copy_from_slice(prefix);
            let mut digits = [0_u8; 10];
            let mut at = digits.len();
            // The number serves only to format the /proc path.
            let mut value = u32::try_from(fd.as_raw_fd()).unwrap_or_else(|_| invariant("sync-fd"));
            loop {
                at -= 1;
                *digits
                    .get_mut(at)
                    .expect("a u32 has at most 10 decimal digits, so at stays within 0..=10") =
                    b'0' + u8::try_from(value % 10).expect("a decimal digit fits in u8");
                value /= 10;
                if value == 0 {
                    break;
                }
            }
            let end = prefix.len() + digits.len() - at;
            path.get_mut(prefix.len()..end)
                .expect("the prefix and at most 10 digits leave room for the final NUL")
                .copy_from_slice(
                    digits
                        .get(at..)
                        .expect("a u32 has at most 10 decimal digits, so at stays within 0..=10"),
                );
            // SAFETY: the stack path is NUL-terminated; link is a writable retained
            // buffer. readlink returns a count, not a C string, and keeps no borrow.
            let length = unsafe {
                libc::readlink(
                    path.as_ptr().cast(),
                    self.link.as_mut_ptr().cast(),
                    self.link.len(),
                )
            };
            let length = usize::try_from(length).unwrap_or_else(|_| invariant("sync-fd"));
            if length == self.link.len() {
                invariant("sync-fd");
            }
            let target = self
                .link
                .get(..length)
                .expect("length is below self.link.len(), checked just above");
            let is_db = target == self.database;
            let is_wal = target
                .strip_suffix(b"-wal")
                .is_some_and(|base| base == self.database);
            if !is_db && !is_wal {
                invariant("sync-fd");
            }
            self.phase = Some(match kind {
                RunKind::Pass { .. } => {
                    if is_db {
                        Phase::Database
                    } else {
                        Phase::Wal
                    }
                }
                RunKind::Restart { .. } => Phase::Restart,
            });
        }

        pub(super) fn phase(&self) -> Phase {
            self.phase.unwrap_or_else(|| invariant("sync-state"))
        }
        fn index(phase: Phase) -> usize {
            match phase {
                Phase::Wal => 0,
                Phase::Database => 2,
                Phase::Restart => 4,
            }
        }
        pub(super) fn failed(&self, phase: Phase) -> bool {
            present(
                self.paths
                    .get(Self::index(phase))
                    .expect("index 0, 2 or 4 of the six paths"),
            )
        }
        pub(super) fn delayed(&self, phase: Phase) -> bool {
            present(
                self.paths
                    .get(Self::index(phase) + 1)
                    .expect("index 1, 3 or 5 of the six paths"),
            )
        }
    }
}
