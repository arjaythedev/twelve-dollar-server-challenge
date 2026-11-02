use crate::http::{
    FrameStatus, Method, parse_http_frame, parse_post_body, trim_js_str, utf16_len_le_500,
    write_usize_ascii,
};
use crate::jwt::{JwtVerifier, parse_positive_i64};
use crate::{
    astraea_eng, astraea_eng_after_batch, astraea_eng_begin_batch, astraea_eng_close,
    astraea_eng_commit_batch, astraea_eng_create, astraea_eng_feed, astraea_eng_get_post,
    astraea_eng_health, astraea_eng_like, astraea_eng_writes_paused, libc,
};
use std::ptr;

const SYS_IO_URING_SETUP: libc::c_long = 425;
const SYS_IO_URING_ENTER: libc::c_long = 426;
const SYS_IO_URING_REGISTER: libc::c_long = 427;

const IORING_SETUP_CQSIZE: u32 = 1 << 3;
const IORING_SETUP_CLAMP: u32 = 1 << 4;
const IORING_SETUP_SUBMIT_ALL: u32 = 1 << 7;
const IORING_SETUP_COOP_TASKRUN: u32 = 1 << 8;
const IORING_SETUP_TASKRUN_FLAG: u32 = 1 << 9;
const IORING_SETUP_SINGLE_ISSUER: u32 = 1 << 12;
const IORING_SETUP_DEFER_TASKRUN: u32 = 1 << 13;

const IORING_ENTER_GETEVENTS: u32 = 1 << 0;
const IORING_ENTER_EXT_ARG: u32 = 1 << 3;

const IORING_SQ_TASKRUN: u32 = 1 << 3;

const IORING_OFF_SQ_RING: i64 = 0;
const IORING_OFF_SQES: i64 = 0x10000000;

const IORING_REGISTER_EVENTFD: u32 = 4;
const IORING_REGISTER_FILES2: u32 = 13;
const IORING_RSRC_REGISTER_SPARSE: u32 = 1 << 0;
const IORING_REGISTER_PBUF_RING: u32 = 22;
const IORING_REGISTER_IOWQ_MAX_WORKERS: u32 = 19;

const IORING_OP_POLL_ADD: u8 = 6;
const IORING_OP_ASYNC_CANCEL: u8 = 14;
const IORING_OP_ACCEPT: u8 = 13;
const IORING_OP_CLOSE: u8 = 19;
const IORING_OP_SEND: u8 = 26;
const IORING_OP_RECV: u8 = 27;

const IOSQE_FIXED_FILE: u8 = 1 << 0;
const IOSQE_IO_HARDLINK: u8 = 1 << 3;
const IOSQE_BUFFER_SELECT: u8 = 1 << 5;
const IOSQE_CQE_SKIP_SUCCESS: u8 = 1 << 6;

const IORING_POLL_ADD_MULTI: u32 = 1 << 0;
const IORING_ACCEPT_MULTISHOT: u16 = 1 << 0;
const IORING_FILE_INDEX_ALLOC: u32 = !0u32;

const IORING_CQE_F_BUFFER: u32 = 1 << 0;
const IORING_CQE_F_MORE: u32 = 1 << 1;
const IORING_CQE_BUFFER_SHIFT: u32 = 16;

const UD_ACCEPT: u64 = 1;
const UD_RECV: u64 = 2;
const UD_SEND: u64 = 3;
const UD_CLOSE: u64 = 4;
const UD_EVFD: u64 = 5;
const UD_CANCEL: u64 = 6;
const UD_POLL: u64 = 7;

#[inline(always)]
const fn pack_ud(op: u64, ring: usize, slot: u32, gen: u32) -> u64 {
    (op << 56) | ((ring as u64) << 48) | ((slot as u64) << 32) | (gen as u64)
}
#[inline(always)]
const fn ud_op(u: u64) -> u64 {
    u >> 56
}
#[inline(always)]
const fn ud_ring(u: u64) -> usize {
    ((u >> 48) & 0xff) as usize
}
#[inline(always)]
const fn ud_slot(u: u64) -> u32 {
    ((u >> 32) & 0xffff) as u32
}
#[inline(always)]
const fn ud_gen(u: u64) -> u32 {
    u as u32
}

#[inline(always)]
const fn pack_cref(ring: usize, slot: u32, gen: u32) -> u64 {
    ((ring as u64) << 48) | ((slot as u64) << 32) | (gen as u64)
}
#[inline(always)]
const fn cref_ring(c: u64) -> usize {
    ((c >> 48) & 0xff) as usize
}
#[inline(always)]
const fn cref_slot(c: u64) -> usize {
    ((c >> 32) & 0xffff) as usize
}
#[inline(always)]
const fn cref_gen(c: u64) -> u32 {
    c as u32
}

const CF_OPEN: u16 = 1 << 0;
const CF_SENDING: u16 = 1 << 1;
const CF_CLOSING: u16 = 1 << 2;
const CF_CLOSE_AFTER: u16 = 1 << 3;
const CF_REPLIED: u16 = 1 << 4;
const CF_READY: u16 = 1 << 5;
const CF_HELD: u16 = 1 << 6;
const CF_REARM: u16 = 1 << 7;
const CF_RQ: u16 = 1 << 8;
const CF_RECVING: u16 = 1 << 9;
const CF_POLLING: u16 = 1 << 10;
const CF_CONTINUED: u16 = 1 << 11;

const NLISTEN: usize = 8;
const ALL_LISTENERS: u8 = ((1u32 << NLISTEN) - 1) as u8;
const ACCEPT_NEXT: u32 = 8192;
const ACCEPT_STOP: u32 = 4096;
const READS_PER_BATCH: usize = 16384;
const ANSWERS_PER_BATCH: usize = 2048;
const BATCH_WAIT: u32 = 64;
const BATCH_WAIT_NS: i64 = 2_000_000;
const IDLE_SECONDS: u32 = 90;
const SWEEP_NS: u64 = 2_000_000_000;
const ARENA_CAP: usize = 1 << 20;
const ARENA_POOL_MAX: usize = 64;
const ARENA_PREWARM: usize = 8;
const LIST_PREWARM: usize = 32768;
const RESP_MAX: usize = 65536;
const PEND_MAX: usize = 65536 + 1_048_576;

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoSqringOffsets {
    head: u32,
    tail: u32,
    ring_mask: u32,
    ring_entries: u32,
    flags: u32,
    dropped: u32,
    array: u32,
    resv1: u32,
    user_addr: u64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoCqringOffsets {
    head: u32,
    tail: u32,
    ring_mask: u32,
    ring_entries: u32,
    overflow: u32,
    cqes: u32,
    flags: u32,
    resv1: u32,
    user_addr: u64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringParams {
    sq_entries: u32,
    cq_entries: u32,
    flags: u32,
    sq_thread_cpu: u32,
    sq_thread_idle: u32,
    features: u32,
    wq_fd: u32,
    resv: [u32; 3],
    sq_off: IoSqringOffsets,
    cq_off: IoCqringOffsets,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringSqe {
    opcode: u8,
    flags: u8,
    ioprio: u16,
    fd: i32,
    off: u64,
    addr: u64,
    len: u32,
    op_flags: u32,
    user_data: u64,
    buf_index: u16,
    personality: u16,
    splice_fd_in: i32,
    addr3: u64,
    resv2: u64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringCqe {
    user_data: u64,
    res: i32,
    flags: u32,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringBuf {
    addr: u64,
    len: u32,
    bid: u16,
    resv: u16,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringBufReg {
    ring_addr: u64,
    ring_entries: u32,
    bgid: u16,
    flags: u16,
    resv: [u64; 3],
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringRsrcRegister {
    nr: u32,
    flags: u32,
    resv2: u64,
    data: u64,
    tags: u64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct KernelTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct IoUringGeteventsArg {
    sigmask: u64,
    sigmask_sz: u32,
    min_wait_usec: u32,
    ts: u64,
}

struct Uring {
    fd: i32,
    nslots: u32,
    sq_head: *const u32,
    sq_tail: *mut u32,
    sq_mask: u32,
    sq_entries: u32,
    sq_flags: *const u32,
    sqes: *mut IoUringSqe,
    sqe_tail: u32,
    cq_head: *mut u32,
    cq_tail: *const u32,
    cq_mask: u32,
    cqes: *const IoUringCqe,
    br_entries: *mut IoUringBuf,
    br_tail: *mut u16,
    br_mask: u16,
    br_local_tail: u16,
    br_count: u32,
    buf_size: u32,
    bufs: *mut u8,
}

impl Uring {
    unsafe fn init(
        sq_entries: u32,
        cq_entries: u32,
        nslots: u32,
        nbufs: u32,
        buf_size: u32,
        defer_taskrun: bool,
    ) -> Result<Self, i32> {
        let mut p = IoUringParams {
            cq_entries,
            flags: IORING_SETUP_CQSIZE
                | IORING_SETUP_CLAMP
                | IORING_SETUP_SUBMIT_ALL
                | IORING_SETUP_SINGLE_ISSUER
                | if defer_taskrun {
                    IORING_SETUP_COOP_TASKRUN
                        | IORING_SETUP_TASKRUN_FLAG
                        | IORING_SETUP_DEFER_TASKRUN
                } else {
                    0
                },
            ..Default::default()
        };
        let mut fd = libc::syscall(SYS_IO_URING_SETUP, sq_entries, &mut p as *mut _) as i32;
        if fd < 0 && defer_taskrun && *libc::__errno_location() == libc::EINVAL {
            p = IoUringParams {
                cq_entries,
                flags: IORING_SETUP_CQSIZE
                    | IORING_SETUP_CLAMP
                    | IORING_SETUP_SUBMIT_ALL
                    | IORING_SETUP_SINGLE_ISSUER
                    | IORING_SETUP_COOP_TASKRUN,
                ..Default::default()
            };
            fd = libc::syscall(SYS_IO_URING_SETUP, sq_entries, &mut p as *mut _) as i32;
        }
        if fd < 0 {
            return Err(*libc::__errno_location());
        }

        let iowq_max: [u32; 2] = [1, 1];
        let _ = libc::syscall(
            SYS_IO_URING_REGISTER,
            fd,
            IORING_REGISTER_IOWQ_MAX_WORKERS,
            iowq_max.as_ptr(),
            2,
        );

        let sq_sz = (p.sq_off.array as usize) + (p.sq_entries as usize) * 4;
        let cq_sz = (p.cq_off.cqes as usize)
            + (p.cq_entries as usize) * std::mem::size_of::<IoUringCqe>();
        let map_sz = sq_sz.max(cq_sz);

        let sq_ptr = libc::mmap(
            ptr::null_mut(),
            map_sz,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED | libc::MAP_POPULATE,
            fd,
            IORING_OFF_SQ_RING,
        );
        if sq_ptr == libc::MAP_FAILED {
            let e = *libc::__errno_location();
            libc::close(fd);
            return Err(e);
        }
        let sq_base = sq_ptr as *mut u8;
        let sq_array = sq_base.add(p.sq_off.array as usize) as *mut u32;
        for i in 0..p.sq_entries {
            *sq_array.add(i as usize) = i;
        }

        let sqes_ptr = libc::mmap(
            ptr::null_mut(),
            (p.sq_entries as usize) * std::mem::size_of::<IoUringSqe>(),
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED | libc::MAP_POPULATE,
            fd,
            IORING_OFF_SQES,
        );
        if sqes_ptr == libc::MAP_FAILED {
            let e = *libc::__errno_location();
            libc::close(fd);
            return Err(e);
        }

        let f_reg = IoUringRsrcRegister {
            nr: nslots,
            flags: IORING_RSRC_REGISTER_SPARSE,
            resv2: 0,
            data: 0,
            tags: 0,
        };
        if libc::syscall(
            SYS_IO_URING_REGISTER,
            fd,
            IORING_REGISTER_FILES2,
            &f_reg as *const _,
            std::mem::size_of::<IoUringRsrcRegister>(),
        ) < 0
        {
            let e = *libc::__errno_location();
            libc::close(fd);
            return Err(e);
        }

        let br_sz = (nbufs as usize) * std::mem::size_of::<IoUringBuf>();
        let br_ptr = libc::mmap(
            ptr::null_mut(),
            br_sz,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_POPULATE,
            -1,
            0,
        );
        let bufs_ptr = libc::mmap(
            ptr::null_mut(),
            (nbufs as usize) * (buf_size as usize),
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
            -1,
            0,
        );
        if br_ptr == libc::MAP_FAILED || bufs_ptr == libc::MAP_FAILED {
            let e = *libc::__errno_location();
            libc::close(fd);
            return Err(e);
        }

        let mut breg = IoUringBufReg {
            ring_addr: br_ptr as u64,
            ring_entries: nbufs,
            bgid: 0,
            flags: 0,
            resv: [0; 3],
        };
        if libc::syscall(
            SYS_IO_URING_REGISTER,
            fd,
            IORING_REGISTER_PBUF_RING,
            &breg as *const _,
            1,
        ) < 0
        {
            if *libc::__errno_location() == libc::EINVAL {
                // Workaround for Ubuntu 6.8.0-136..147 inverted resv check (LP #2162843)
                breg.resv[0] = 1;
                if libc::syscall(
                    SYS_IO_URING_REGISTER,
                    fd,
                    IORING_REGISTER_PBUF_RING,
                    &breg as *const _,
                    1,
                ) < 0
                {
                    let e = *libc::__errno_location();
                    libc::close(fd);
                    return Err(e);
                }
            } else {
                let e = *libc::__errno_location();
                libc::close(fd);
                return Err(e);
            }
        }

        let br_entries = br_ptr as *mut IoUringBuf;
        let br_tail = (br_ptr as *mut u8).add(14) as *mut u16;
        let bufs = bufs_ptr as *mut u8;
        let br_mask = (nbufs - 1) as u16;
        for i in 0..nbufs {
            let idx = (i as u16 & br_mask) as usize;
            *br_entries.add(idx) = IoUringBuf {
                addr: bufs.add((i * buf_size) as usize) as u64,
                len: buf_size,
                bid: i as u16,
                resv: 0,
            };
        }
        std::sync::atomic::AtomicU16::from_ptr(br_tail)
            .store(nbufs as u16, std::sync::atomic::Ordering::Release);

        let sq_tail_ptr = sq_base.add(p.sq_off.tail as usize) as *mut u32;
        Ok(Self {
            fd,
            nslots,
            sq_head: sq_base.add(p.sq_off.head as usize) as *const u32,
            sq_tail: sq_tail_ptr,
            sq_mask: *(sq_base.add(p.sq_off.ring_mask as usize) as *const u32),
            sq_entries: *(sq_base.add(p.sq_off.ring_entries as usize) as *const u32),
            sq_flags: sq_base.add(p.sq_off.flags as usize) as *const u32,
            sqes: sqes_ptr as *mut IoUringSqe,
            sqe_tail: *sq_tail_ptr,
            cq_head: sq_base.add(p.cq_off.head as usize) as *mut u32,
            cq_tail: sq_base.add(p.cq_off.tail as usize) as *const u32,
            cq_mask: *(sq_base.add(p.cq_off.ring_mask as usize) as *const u32),
            cqes: sq_base.add(p.cq_off.cqes as usize) as *const IoUringCqe,
            br_entries,
            br_tail,
            br_mask,
            br_local_tail: nbufs as u16,
            br_count: nbufs,
            buf_size,
            bufs,
        })
    }

    #[inline(always)]
    unsafe fn register_eventfd(&self) -> Result<i32, i32> {
        let efd = libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC);
        if efd < 0 {
            return Err(*libc::__errno_location());
        }
        if libc::syscall(
            SYS_IO_URING_REGISTER,
            self.fd,
            IORING_REGISTER_EVENTFD,
            &efd as *const i32,
            1,
        ) < 0
        {
            let e = *libc::__errno_location();
            libc::close(efd);
            return Err(e);
        }
        Ok(efd)
    }

    #[inline(always)]
    unsafe fn buf_ptr(&self, bid: u16) -> *const u8 {
        self.bufs.add((bid as usize) * (self.buf_size as usize))
    }

    #[inline(always)]
    unsafe fn buf_return(&mut self, bid: u16) {
        let idx = (self.br_local_tail & self.br_mask) as usize;
        *self.br_entries.add(idx) = IoUringBuf {
            addr: self.buf_ptr(bid) as u64,
            len: self.buf_size,
            bid,
            resv: 0,
        };
        self.br_local_tail = self.br_local_tail.wrapping_add(1);
    }

    #[inline(always)]
    unsafe fn buf_publish(&mut self) {
        std::sync::atomic::AtomicU16::from_ptr(self.br_tail)
            .store(self.br_local_tail, std::sync::atomic::Ordering::Release);
    }

    #[inline(always)]
    unsafe fn pending_submit(&self) -> u32 {
        self.sqe_tail.wrapping_sub(
            std::sync::atomic::AtomicU32::from_ptr(self.sq_tail)
                .load(std::sync::atomic::Ordering::Relaxed),
        )
    }

    #[inline(always)]
    unsafe fn unconsumed_sqes(&self) -> u32 {
        self.sqe_tail.wrapping_sub(
            std::sync::atomic::AtomicU32::from_ptr(self.sq_head as *mut u32)
                .load(std::sync::atomic::Ordering::Acquire),
        )
    }

    #[inline(always)]
    unsafe fn submit_raw(&mut self, min_complete: u32, flags: u32, arg: *const libc::c_void, argsz: usize) -> i32 {
        let to_submit = self.pending_submit();
        std::sync::atomic::AtomicU32::from_ptr(self.sq_tail)
            .store(self.sqe_tail, std::sync::atomic::Ordering::Release);
        let r = libc::syscall(
            SYS_IO_URING_ENTER,
            self.fd,
            to_submit,
            min_complete,
            flags,
            arg,
            argsz,
        ) as i32;
        if r < 0 {
            -*libc::__errno_location()
        } else {
            r
        }
    }

    #[inline(always)]
    unsafe fn wait(&mut self, min_complete: u32, timeout_ns: i64) -> i32 {
        let to_submit = self.pending_submit();
        std::sync::atomic::AtomicU32::from_ptr(self.sq_tail)
            .store(self.sqe_tail, std::sync::atomic::Ordering::Release);
        if min_complete == 0 {
            let flags = if (std::sync::atomic::AtomicU32::from_ptr(self.sq_flags as *mut u32)
                .load(std::sync::atomic::Ordering::Relaxed)
                & IORING_SQ_TASKRUN)
                != 0
            {
                IORING_ENTER_GETEVENTS
            } else {
                0
            };
            if to_submit == 0 && flags == 0 {
                return 0;
            }
            let r = libc::syscall(
                SYS_IO_URING_ENTER,
                self.fd,
                to_submit,
                0u32,
                flags,
                ptr::null::<libc::c_void>(),
                0usize,
            ) as i32;
            return if r < 0 {
                -*libc::__errno_location()
            } else {
                r
            };
        }
        let ts = KernelTimespec {
            tv_sec: timeout_ns / 1_000_000_000,
            tv_nsec: timeout_ns % 1_000_000_000,
        };
        let arg = IoUringGeteventsArg {
            sigmask: 0,
            sigmask_sz: 8,
            min_wait_usec: 0,
            ts: &ts as *const KernelTimespec as u64,
        };
        let r = libc::syscall(
            SYS_IO_URING_ENTER,
            self.fd,
            to_submit,
            min_complete,
            IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
            &arg as *const IoUringGeteventsArg,
            std::mem::size_of::<IoUringGeteventsArg>(),
        ) as i32;
        if r < 0 {
            -*libc::__errno_location()
        } else {
            r
        }
    }

    #[inline(always)]
    unsafe fn get_sqe(&mut self) -> *mut IoUringSqe {
        if self.unconsumed_sqes() >= self.sq_entries {
            self.buf_publish();
            let _ = self.submit_raw(0, 0, ptr::null(), 0);
            while self.unconsumed_sqes() >= self.sq_entries {
                let _ = self.submit_raw(0, IORING_ENTER_GETEVENTS, ptr::null(), 0);
            }
        }
        let idx = (self.sqe_tail & self.sq_mask) as usize;
        self.sqe_tail = self.sqe_tail.wrapping_add(1);
        let sqe = self.sqes.add(idx);
        *sqe = IoUringSqe::default();
        sqe
    }
}

#[repr(C)]
struct Arena {
    next: *mut Arena,
    refs: u32,
    used: u32,
    cap: u32,
    _pad: u32,
    data: [u8; 0],
}

impl Arena {
    #[inline(always)]
    unsafe fn data_ptr(this: *mut Arena) -> *mut u8 {
        (this as *mut u8).add(std::mem::size_of::<Arena>())
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Conn {
    gen: u32,
    flags: u16,
    ring: u16,
    slot: u32,
    last_active: u32,
    pend: *mut u8,
    pend_len: u32,
    pend_cap: u32,
    send_arena: *mut Arena,
}

#[derive(Clone, Copy)]
struct Reply {
    cref: u64,
    arena: *mut Arena,
    off: u32,
    len: u32,
    db: bool,
}

struct RingState {
    r: Uring,
    conns: *mut Conn,
    efd: i32,
    open: u32,
    accept_armed: u8,
    accept_cancelling: u8,
    populated: bool,
    rearm: Vec<u32>,
    reads: u32,
}

pub struct Server {
    rings: Vec<RingState>,
    nr_max: usize,
    ring_slots: u32,
    ring_bufs: u32,
    ring_buf_size: u32,
    ring_grow_failed: bool,
    listen_fds: [i32; NLISTEN],
    eng: *mut astraea_eng,
    jwt: JwtVerifier,
    start_instant: std::time::Instant,
    now_s: u32,
    unix_now_s: f64,
    writes_ok: bool,
    batch_tx_open: bool,
    pool: *mut Arena,
    pool_n: usize,
    cur_arena: *mut Arena,
    batch_arenas: Vec<*mut Arena>,
    replies: Vec<Reply>,
    ready: Vec<u64>,
    held: Vec<u64>,
    rq: Vec<u64>,
    rq_head: usize,
    answers_left: usize,
    scratch: Vec<u8>,
}

const R_400_BAD: &[u8] = b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 29\r\nConnection: close\r\n\r\n{\"error\":\"malformed request\"}";
const R_500: &[u8] = b"HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";
const R_100_CONTINUE: &[u8] = b"HTTP/1.1 100 Continue\r\n\r\n";

#[inline]
fn create_listen_socket(host: &str, port: u16, backlog: i32) -> Result<i32, String> {
    unsafe {
        let fd = libc::socket(
            libc::AF_INET,
            libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            0,
        );
        if fd < 0 {
            return Err("socket() failed".into());
        }
        let one: libc::c_int = 1;
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_REUSEADDR,
            &one as *const _ as *const libc::c_void,
            4,
        );
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_REUSEPORT,
            &one as *const _ as *const libc::c_void,
            4,
        );
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_NODELAY,
            &one as *const _ as *const libc::c_void,
            4,
        );

        let ip: std::net::Ipv4Addr = if host == "localhost" {
            std::net::Ipv4Addr::LOCALHOST
        } else {
            host.parse().unwrap_or(std::net::Ipv4Addr::UNSPECIFIED)
        };
        let addr = libc::sockaddr_in {
            sin_family: libc::AF_INET as u16,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr {
                s_addr: u32::from_ne_bytes(ip.octets()),
            },
            sin_zero: [0; 8],
        };
        for attempt in 0..=300 {
            if libc::bind(
                fd,
                &addr as *const libc::sockaddr_in as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_in>() as u32,
            ) == 0
            {
                break;
            }
            if *libc::__errno_location() != libc::EADDRINUSE || attempt == 300 {
                libc::close(fd);
                return Err("bind() failed".into());
            }
            libc::usleep(100_000);
        }
        if libc::listen(fd, backlog) < 0 {
            libc::close(fd);
            return Err("listen() failed".into());
        }
        Ok(fd)
    }
}

#[inline(always)]
fn now_monotonic_ns() -> u64 {
    unsafe {
        let mut ts = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts);
        (ts.tv_sec as u64) * 1_000_000_000 + (ts.tv_nsec as u64)
    }
}

#[inline(always)]
fn now_realtime_secs() -> f64 {
    unsafe {
        let mut ts = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        libc::clock_gettime(libc::CLOCK_REALTIME, &mut ts);
        (ts.tv_sec as f64) + (ts.tv_nsec as f64) * 1e-9
    }
}

impl Server {
    pub fn new(
        host: &str,
        port: u16,
        eng: *mut astraea_eng,
        secret: &[u8],
        ring_slots: u32,
        nr_max: usize,
    ) -> Result<Self, String> {
        let mut listen_fds = [-1i32; NLISTEN];
        for i in 0..NLISTEN {
            listen_fds[i] = create_listen_socket(host, port, 4096)?;
        }

        let mut srv = Self {
            rings: Vec::with_capacity(nr_max),
            nr_max,
            ring_slots,
            ring_bufs: 4096,
            ring_buf_size: 1024,
            ring_grow_failed: false,
            listen_fds,
            eng,
            jwt: JwtVerifier::new(secret),
            start_instant: std::time::Instant::now(),
            now_s: 0,
            unix_now_s: now_realtime_secs(),
            writes_ok: true,
            batch_tx_open: false,
            pool: ptr::null_mut(),
            pool_n: 0,
            cur_arena: ptr::null_mut(),
            batch_arenas: Vec::with_capacity(256),
            replies: Vec::with_capacity(LIST_PREWARM),
            ready: Vec::with_capacity(LIST_PREWARM),
            held: Vec::with_capacity(LIST_PREWARM),
            rq: Vec::with_capacity(LIST_PREWARM),
            rq_head: 0,
            answers_left: ANSWERS_PER_BATCH,
            scratch: vec![0u8; RESP_MAX],
        };

        unsafe {
            srv.prewarm_memory();
            if !srv.ring_create(0) {
                return Err("failed to initialize io_uring ring 0".into());
            }
            srv.accept_policy();
        }
        Ok(srv)
    }

    unsafe fn prewarm_memory(&mut self) {
        let total_sz = std::mem::size_of::<Arena>() + ARENA_CAP;
        for _ in 0..ARENA_PREWARM {
            let a = libc::malloc(total_sz) as *mut Arena;
            if a.is_null() {
                std::process::abort();
            }
            ptr::write_bytes(a as *mut u8, 0, total_sz);
            (*a).cap = ARENA_CAP as u32;
            (*a).next = self.pool;
            self.pool = a;
            self.pool_n += 1;
        }
    }

    #[inline(always)]
    unsafe fn arena_get(&mut self) -> *mut Arena {
        let a = self.pool;
        if !a.is_null() {
            self.pool = (*a).next;
            self.pool_n -= 1;
        } else {
            let total_sz = std::mem::size_of::<Arena>() + ARENA_CAP;
            let new_a = libc::malloc(total_sz) as *mut Arena;
            if new_a.is_null() {
                std::process::abort();
            }
            (*new_a).cap = ARENA_CAP as u32;
            (*new_a).refs = 0;
            (*new_a).used = 0;
            return new_a;
        }
        (*a).refs = 0;
        (*a).used = 0;
        a
    }

    #[inline(always)]
    unsafe fn arena_unref(&mut self, a: *mut Arena) {
        (*a).refs -= 1;
        if (*a).refs != 0 {
            return;
        }
        if self.pool_n < ARENA_POOL_MAX {
            (*a).next = self.pool;
            self.pool = a;
            self.pool_n += 1;
        } else {
            libc::free(a as *mut libc::c_void);
        }
    }

    #[inline(always)]
    unsafe fn arena_reserve(&mut self, n: usize) -> *mut u8 {
        if self.cur_arena.is_null()
            || ((*self.cur_arena).used as usize) + n > ((*self.cur_arena).cap as usize)
        {
            let a = self.arena_get();
            (*a).refs = 1;
            self.batch_arenas.push(a);
            self.cur_arena = a;
        }
        Arena::data_ptr(self.cur_arena).add((*self.cur_arena).used as usize)
    }

    unsafe fn ring_create(&mut self, idx: usize) -> bool {
        let mut cq = 65536u32;
        let r = loop {
            match Uring::init(
                4096,
                cq,
                self.ring_slots,
                self.ring_bufs,
                self.ring_buf_size,
                idx == 0,
            ) {
                Ok(ring) => break ring,
                Err(e) if e == libc::ENOMEM && cq > 8192 => {
                    cq /= 2;
                }
                Err(_) => return false,
            }
        };

        let conns_bytes = (self.ring_slots as usize) * std::mem::size_of::<Conn>();
        let conns_ptr = libc::mmap(
            ptr::null_mut(),
            conns_bytes,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
            -1,
            0,
        );
        if conns_ptr == libc::MAP_FAILED {
            libc::close(r.fd);
            return false;
        }

        let mut efd = -1;
        if idx > 0 {
            match r.register_eventfd() {
                Ok(fd) => efd = fd,
                Err(_) => {
                    libc::munmap(conns_ptr, conns_bytes);
                    libc::close(r.fd);
                    return false;
                }
            }
            let sqe = self.rings[0].r.get_sqe();
            (*sqe).opcode = IORING_OP_POLL_ADD;
            (*sqe).fd = efd;
            (*sqe).len = IORING_POLL_ADD_MULTI;
            (*sqe).op_flags = libc::POLLIN as u32;
            (*sqe).user_data = pack_ud(UD_EVFD, idx, 0, 0);
        }

        self.rings.push(RingState {
            r,
            conns: conns_ptr as *mut Conn,
            efd,
            open: 0,
            accept_armed: 0,
            accept_cancelling: 0,
            populated: false,
            rearm: Vec::with_capacity(4096),
            reads: 0,
        });
        true
    }

    unsafe fn ring_populate(&mut self, ri: usize) {
        let rs = &mut self.rings[ri];
        if rs.populated {
            return;
        }
        let nc = (rs.r.nslots as usize) * std::mem::size_of::<Conn>();
        let nb = (rs.r.br_count as usize) * (rs.r.buf_size as usize);
        let mut p = 0usize;
        while p < nc {
            ptr::write_volatile((rs.conns as *mut u8).add(p), 0);
            p += 4096;
        }
        let mut b = 0usize;
        while b < nb {
            ptr::write_volatile(rs.r.bufs.add(b), 0);
            b += 4096;
        }
        rs.populated = true;
    }

    unsafe fn accept_policy(&mut self) {
        let nr = self.rings.len();
        let stop_margin = ACCEPT_STOP.min(self.ring_slots / 8).max(16);
        let next_margin = ACCEPT_NEXT.min(self.ring_slots / 4).max(32);
        if nr < self.nr_max
            && !self.ring_grow_failed
            && self.rings[nr - 1].open + next_margin >= self.rings[nr - 1].r.nslots
        {
            if !self.ring_create(nr) {
                self.ring_grow_failed = true;
            }
        }
        let nr = self.rings.len();
        let mut lowest = nr;
        for i in 0..nr {
            if self.rings[i].open + stop_margin < self.rings[i].r.nslots {
                lowest = i;
                break;
            }
        }
        let lowest_nearly_full = lowest < nr
            && self.rings[lowest].open + next_margin >= self.rings[lowest].r.nslots;

        for i in 0..nr {
            let room = self.rings[i].open + stop_margin < self.rings[i].r.nslots;
            let want = room && (i == lowest || (i == lowest + 1 && lowest_nearly_full));
            if want && self.rings[i].accept_armed != ALL_LISTENERS {
                self.ring_populate(i);
                let rs = &mut self.rings[i];
                for l in 0..NLISTEN {
                    let bit = 1u8 << l;
                    if (rs.accept_armed & bit) == 0 {
                        let sqe = rs.r.get_sqe();
                        (*sqe).opcode = IORING_OP_ACCEPT;
                        (*sqe).fd = self.listen_fds[l];
                        (*sqe).ioprio = IORING_ACCEPT_MULTISHOT;
                        (*sqe).splice_fd_in = IORING_FILE_INDEX_ALLOC as i32;
                        (*sqe).user_data = pack_ud(UD_ACCEPT, i, l as u32, 0);
                        rs.accept_armed |= bit;
                    }
                }
            } else if !room {
                let rs = &mut self.rings[i];
                for l in 0..NLISTEN {
                    let bit = 1u8 << l;
                    if ((rs.accept_armed & !rs.accept_cancelling) & bit) != 0 {
                        let sqe = rs.r.get_sqe();
                        (*sqe).opcode = IORING_OP_ASYNC_CANCEL;
                        (*sqe).fd = -1;
                        (*sqe).flags = IOSQE_CQE_SKIP_SUCCESS;
                        (*sqe).addr = pack_ud(UD_ACCEPT, i, l as u32, 0);
                        (*sqe).user_data = pack_ud(UD_CANCEL, i, 0, 0);
                        rs.accept_cancelling |= bit;
                    }
                }
            }
        }
    }

    #[inline(always)]
    unsafe fn arm_poll(&mut self, ri: usize, slot: u32) {
        let rs = &mut self.rings[ri];
        let c = &mut *rs.conns.add(slot as usize);
        let sqe = rs.r.get_sqe();
        (*sqe).opcode = IORING_OP_POLL_ADD;
        (*sqe).flags = IOSQE_FIXED_FILE;
        (*sqe).fd = slot as i32;
        (*sqe).len = IORING_POLL_ADD_MULTI;
        (*sqe).op_flags = (libc::POLLIN | libc::POLLRDHUP) as u32;
        (*sqe).user_data = pack_ud(UD_POLL, ri, slot, c.gen);
        c.flags = (c.flags & !CF_REARM) | CF_POLLING;
    }

    #[inline(always)]
    unsafe fn conn_close(&mut self, ri: usize, slot: u32) {
        let rs = &mut self.rings[ri];
        let c = &mut *rs.conns.add(slot as usize);
        if (c.flags & CF_OPEN) == 0 || (c.flags & CF_CLOSING) != 0 {
            return;
        }
        c.flags |= CF_CLOSING;
        if rs.r.unconsumed_sqes() + 3 >= rs.r.sq_entries {
            rs.r.buf_publish();
            let _ = rs.r.submit_raw(0, 0, ptr::null(), 0);
        }
        if (c.flags & CF_POLLING) != 0 {
            c.flags &= !CF_POLLING;
            let sqe_cancel = rs.r.get_sqe();
            (*sqe_cancel).opcode = IORING_OP_ASYNC_CANCEL;
            (*sqe_cancel).fd = -1;
            (*sqe_cancel).flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
            (*sqe_cancel).addr = pack_ud(UD_POLL, ri, slot, c.gen);
            (*sqe_cancel).user_data = pack_ud(UD_CANCEL, ri, slot, c.gen);
        }
        if (c.flags & CF_SENDING) != 0 {
            let sqe_cancel_send = rs.r.get_sqe();
            (*sqe_cancel_send).opcode = IORING_OP_ASYNC_CANCEL;
            (*sqe_cancel_send).fd = -1;
            (*sqe_cancel_send).flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
            (*sqe_cancel_send).addr = pack_ud(UD_SEND, ri, slot, c.gen);
            (*sqe_cancel_send).user_data = pack_ud(UD_CANCEL, ri, slot, c.gen);
        }
        let sqe_close = rs.r.get_sqe();
        (*sqe_close).opcode = IORING_OP_CLOSE;
        (*sqe_close).splice_fd_in = (slot + 1) as i32;
        (*sqe_close).user_data = pack_ud(UD_CLOSE, ri, slot, c.gen);
    }

    #[inline(always)]
    unsafe fn conn_freed(&mut self, ri: usize, slot: u32) {
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        c.gen = c.gen.wrapping_add(1);
        c.flags = 0;
        if !c.pend.is_null() {
            libc::free(c.pend as *mut libc::c_void);
            c.pend = ptr::null_mut();
        }
        c.pend_len = 0;
        c.pend_cap = 0;
        let sa = c.send_arena;
        c.send_arena = ptr::null_mut();
        if !sa.is_null() {
            self.arena_unref(sa);
        }
        self.rings[ri].open -= 1;
    }

    #[inline(always)]
    unsafe fn queue_read(&mut self, ri: usize, slot: u32) {
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        if (c.flags & CF_RQ) != 0 {
            return;
        }
        c.flags |= CF_RQ;
        self.rq.push(pack_cref(ri, slot, c.gen));
    }

    unsafe fn issue_reads(&mut self) -> usize {
        let mut budget = READS_PER_BATCH;
        let end = self.rq.len();
        while self.rq_head < end && budget > 0 {
            let cref = self.rq[self.rq_head];
            self.rq_head += 1;
            let ri = cref_ring(cref);
            let slot = cref_slot(cref);
            let gen = cref_gen(cref);
            let rs = &mut self.rings[ri];
            let c = &mut *rs.conns.add(slot);
            if c.gen != gen || (c.flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN {
                continue;
            }
            if (c.flags & CF_RECVING) != 0 || rs.reads >= rs.r.br_count {
                self.rq.push(cref);
                continue;
            }
            c.flags = (c.flags & !CF_RQ) | CF_RECVING;
            rs.reads += 1;
            budget -= 1;
            let sqe = rs.r.get_sqe();
            (*sqe).opcode = IORING_OP_RECV;
            (*sqe).flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
            (*sqe).fd = slot as i32;
            (*sqe).op_flags = libc::MSG_DONTWAIT as u32;
            (*sqe).buf_index = 0;
            (*sqe).user_data = pack_ud(UD_RECV, ri, slot as u32, c.gen);
        }
        if self.rq_head == self.rq.len() {
            self.rq.clear();
            self.rq_head = 0;
        } else if self.rq_head > self.rq.len() / 2 {
            self.rq.copy_within(self.rq_head.., 0);
            let new_len = self.rq.len() - self.rq_head;
            self.rq.truncate(new_len);
            self.rq_head = 0;
        }
        READS_PER_BATCH - budget
    }

    unsafe fn pend_append(c: &mut Conn, data: &[u8]) {
        let need = (c.pend_len as usize) + data.len();
        if need > (c.pend_cap as usize) {
            let mut cap = if c.pend_cap != 0 {
                c.pend_cap as usize
            } else {
                512
            };
            while cap < need {
                cap *= 2;
            }
            let np = libc::realloc(c.pend as *mut libc::c_void, cap) as *mut u8;
            if np.is_null() {
                std::process::abort();
            }
            c.pend = np;
            c.pend_cap = cap as u32;
        }
        ptr::copy_nonoverlapping(data.as_ptr(), c.pend.add(c.pend_len as usize), data.len());
        c.pend_len = need as u32;
    }

    unsafe fn stash(c: &mut Conn, p: *const u8, n: usize) {
        if n == 0 {
            c.pend_len = 0;
            if !c.pend.is_null() {
                libc::free(c.pend as *mut libc::c_void);
                c.pend = ptr::null_mut();
                c.pend_cap = 0;
            }
            return;
        }
        if p == c.pend {
            c.pend_len = n as u32;
            return;
        }
        if !c.pend.is_null()
            && (p as usize) >= (c.pend as usize)
            && (p as usize) < (c.pend as usize) + (c.pend_cap as usize)
        {
            ptr::copy(p, c.pend, n);
            c.pend_len = n as u32;
            return;
        }
        if (c.pend_cap as usize) < n {
            let mut cap = if c.pend_cap != 0 {
                c.pend_cap as usize
            } else {
                512
            };
            while cap < n {
                cap *= 2;
            }
            let np = libc::realloc(c.pend as *mut libc::c_void, cap) as *mut u8;
            if np.is_null() {
                std::process::abort();
            }
            c.pend = np;
            c.pend_cap = cap as u32;
        }
        ptr::copy_nonoverlapping(p, c.pend, n);
        c.pend_len = n as u32;
    }

    #[inline(always)]
    unsafe fn ensure_batch_tx(&mut self) {
        if !self.batch_tx_open {
            astraea_eng_begin_batch(self.eng);
            self.batch_tx_open = true;
        }
    }

    unsafe fn format_http_into_arena(
        &mut self,
        status: i32,
        body: &[u8],
        close: bool,
        prepend_continue: bool,
    ) -> (*mut Arena, u32, u32) {
        let status_head: &[u8] = match (status, close) {
            (200, false) => b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ",
            (201, false) => b"HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ",
            (400, false) => b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ",
            (401, false) => b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ",
            (404, false) => b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ",
            (200, true) => b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ",
            (201, true) => b"HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ",
            (400, true) => b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ",
            (401, true) => b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ",
            (404, true) => b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ",
            (_, false) => b"HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ",
            (_, true) => b"HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ",
        };

        let mut cl_buf = [0u8; 20];
        let cl_digits = write_usize_ascii(body.len(), &mut cl_buf);
        let cont_len = if prepend_continue {
            R_100_CONTINUE.len()
        } else {
            0
        };
        let total = cont_len + status_head.len() + cl_digits.len() + 4 + body.len();
        let dst = self.arena_reserve(total);
        let base_off = (*self.cur_arena).used;
        let mut p = dst;
        if prepend_continue {
            ptr::copy_nonoverlapping(R_100_CONTINUE.as_ptr(), p, R_100_CONTINUE.len());
            p = p.add(R_100_CONTINUE.len());
        }
        ptr::copy_nonoverlapping(status_head.as_ptr(), p, status_head.len());
        p = p.add(status_head.len());
        ptr::copy_nonoverlapping(cl_digits.as_ptr(), p, cl_digits.len());
        p = p.add(cl_digits.len());
        ptr::copy_nonoverlapping(b"\r\n\r\n".as_ptr(), p, 4);
        p = p.add(4);
        ptr::copy_nonoverlapping(body.as_ptr(), p, body.len());
        (*self.cur_arena).used += total as u32;
        (self.cur_arena, base_off, total as u32)
    }

    #[cold]
    fn post_error_body(path: &[u8], status: i32, msg: &'static [u8]) -> (i32, &'static [u8]) {
        if let Some(rest) = path.strip_prefix(b"/posts/") {
            if let Some(mid) = rest.strip_suffix(b"/like") {
                if mid.contains(&b'/') {
                    return (404, b"{\"error\":\"not found\"}");
                }
            }
        }
        (status, msg)
    }

    unsafe fn conn_feed(&mut self, ri: usize, slot: u32, data: *const u8, len: usize) {
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        let (mut buf_ptr, mut n) = if c.pend_len != 0 {
            if len != 0 {
                let slice = std::slice::from_raw_parts(data, len);
                Self::pend_append(c, slice);
            }
            (c.pend as *const u8, c.pend_len as usize)
        } else {
            (data, len)
        };

        while n > 0 {
            if (c.flags & (CF_SENDING | CF_REPLIED | CF_CLOSING | CF_HELD)) != 0 {
                break;
            }
            let input_slice = std::slice::from_raw_parts(buf_ptr, n);
            match parse_http_frame(input_slice) {
                Ok(FrameStatus::Incomplete { expect_continue }) => {
                    if n > PEND_MAX {
                        self.conn_close(ri, slot);
                        n = 0;
                    } else if expect_continue && (c.flags & CF_CONTINUED) == 0 {
                        c.flags |= CF_CONTINUED;
                        let dst = self.arena_reserve(R_100_CONTINUE.len());
                        let off = (*self.cur_arena).used;
                        ptr::copy_nonoverlapping(
                            R_100_CONTINUE.as_ptr(),
                            dst,
                            R_100_CONTINUE.len(),
                        );
                        (*self.cur_arena).used += R_100_CONTINUE.len() as u32;
                        self.replies.push(Reply {
                            cref: pack_cref(ri, slot, c.gen),
                            arena: self.cur_arena,
                            off,
                            len: R_100_CONTINUE.len() as u32,
                            db: false,
                        });
                        c.flags |= CF_REPLIED;
                    }
                    break;
                }
                Err(()) => {
                    let dst = self.arena_reserve(R_400_BAD.len());
                    let off = (*self.cur_arena).used;
                    ptr::copy_nonoverlapping(R_400_BAD.as_ptr(), dst, R_400_BAD.len());
                    (*self.cur_arena).used += R_400_BAD.len() as u32;
                    self.replies.push(Reply {
                        cref: pack_cref(ri, slot, c.gen),
                        arena: self.cur_arena,
                        off,
                        len: R_400_BAD.len() as u32,
                        db: false,
                    });
                    c.flags |= CF_REPLIED | CF_CLOSE_AFTER;
                    n = 0;
                    break;
                }
                Ok(FrameStatus::Complete(frame)) => {
                    c.flags &= !CF_CONTINUED;
                    let mut is_db = false;
                    let status: i32;
                    let mut out_len: usize = 0;
                    let mut static_err: Option<&'static [u8]> = None;

                    let path = frame.path;
                    match frame.method {
                        Method::Get => {
                            if path == b"/feed" {
                                self.ensure_batch_tx();
                                is_db = true;
                                status = astraea_eng_feed(
                                    self.eng,
                                    self.scratch.as_mut_ptr() as *mut i8,
                                    self.scratch.len(),
                                    &mut out_len,
                                );
                            } else if let Some(s) = path.strip_prefix(b"/posts/") {
                                if let Ok(s_str) = std::str::from_utf8(s) {
                                    if let Some(id) = parse_positive_i64(s_str) {
                                        self.ensure_batch_tx();
                                        is_db = true;
                                        status = astraea_eng_get_post(
                                            self.eng,
                                            id,
                                            self.scratch.as_mut_ptr() as *mut i8,
                                            self.scratch.len(),
                                            &mut out_len,
                                        );
                                    } else if !s.contains(&b'/') {
                                        status = 400;
                                        static_err = Some(b"{\"error\":\"invalid post id\"}");
                                    } else {
                                        status = 404;
                                        static_err = Some(b"{\"error\":\"not found\"}");
                                    }
                                } else {
                                    status = 400;
                                    static_err = Some(b"{\"error\":\"invalid post id\"}");
                                }
                            } else if path == b"/health" {
                                self.ensure_batch_tx();
                                is_db = true;
                                let up = self.start_instant.elapsed().as_secs() as i64;
                                status = astraea_eng_health(
                                    self.eng,
                                    up,
                                    self.scratch.as_mut_ptr() as *mut i8,
                                    self.scratch.len(),
                                    &mut out_len,
                                );
                            } else {
                                status = 404;
                                static_err = Some(b"{\"error\":\"not found\"}");
                            }
                        }
                        Method::Post => {
                            let is_posts = path == b"/posts";
                            let is_like =
                                path.starts_with(b"/posts/") && path.ends_with(b"/like");
                            if is_posts || is_like {
                                if !self.writes_ok {
                                    c.flags |= CF_HELD;
                                    self.held.push(pack_cref(ri, slot, c.gen));
                                    break;
                                }
                                match self.jwt.verify(frame.authorization, self.unix_now_s) {
                                    Err(msg) => {
                                        let err_json: &'static [u8] = match msg {
                                            "missing bearer token" => {
                                                b"{\"error\":\"missing bearer token\"}"
                                            }
                                            "invalid token payload" => {
                                                b"{\"error\":\"invalid token payload\"}"
                                            }
                                            _ => b"{\"error\":\"invalid or expired token\"}",
                                        };
                                        let (st, b) = Self::post_error_body(path, 401, err_json);
                                        status = st;
                                        static_err = Some(b);
                                    }
                                    Ok((uid, username)) => {
                                        let uname_ptr = username.as_ptr() as *const i8;
                                        let uname_len = username.len();
                                        if is_posts {
                                            match parse_post_body(&frame.body) {
                                                Err(_) => {
                                                    status = 400;
                                                    static_err =
                                                        Some(b"{\"error\":\"malformed JSON body\"}");
                                                }
                                                Ok(None) => {
                                                    status = 400;
                                                    static_err =
                                                        Some(b"{\"error\":\"body is required\"}");
                                                }
                                                Ok(Some(raw_body)) => {
                                                    let trimmed = trim_js_str(&raw_body);
                                                    if trimmed.is_empty() {
                                                        status = 400;
                                                        static_err = Some(
                                                            b"{\"error\":\"body is required\"}",
                                                        );
                                                    } else if !utf16_len_le_500(trimmed) {
                                                        status = 400;
                                                        static_err = Some(b"{\"error\":\"body must be at most 500 characters\"}");
                                                    } else {
                                                        self.ensure_batch_tx();
                                                        is_db = true;
                                                        let body_b = trimmed.as_bytes();
                                                        status = astraea_eng_create(
                                                            self.eng,
                                                            uid,
                                                            uname_ptr,
                                                            uname_len,
                                                            body_b.as_ptr() as *const i8,
                                                            body_b.len(),
                                                            self.scratch.as_mut_ptr() as *mut i8,
                                                            self.scratch.len(),
                                                            &mut out_len,
                                                        );
                                                    }
                                                }
                                            }
                                        } else {
                                            let mid = if path.len() >= 12 {
                                                &path[7..path.len() - 5]
                                            } else {
                                                b""
                                            };
                                            let id_opt = std::str::from_utf8(mid)
                                                .ok()
                                                .and_then(parse_positive_i64);
                                            if let Some(pid) = id_opt {
                                                self.ensure_batch_tx();
                                                is_db = true;
                                                status = astraea_eng_like(
                                                    self.eng,
                                                    uid,
                                                    pid,
                                                    self.scratch.as_mut_ptr() as *mut i8,
                                                    self.scratch.len(),
                                                    &mut out_len,
                                                );
                                            } else {
                                                let (st, b) = Self::post_error_body(
                                                    path,
                                                    400,
                                                    b"{\"error\":\"invalid post id\"}",
                                                );
                                                status = st;
                                                static_err = Some(b);
                                            }
                                        }
                                    }
                                }
                            } else {
                                status = 404;
                                static_err = Some(b"{\"error\":\"not found\"}");
                            }
                        }
                        Method::Other => {
                            status = 404;
                            static_err = Some(b"{\"error\":\"not found\"}");
                        }
                    }

                    let consumed = frame.consumed;
                    let close = frame.close;
                    let (arena, off, resp_len) = if let Some(err_bytes) = static_err {
                        self.format_http_into_arena(status, err_bytes, close, false)
                    } else {
                        let body_slice =
                            std::slice::from_raw_parts(self.scratch.as_ptr(), out_len);
                        self.format_http_into_arena(status, body_slice, close, false)
                    };

                    let c = &mut *self.rings[ri].conns.add(slot as usize);
                    self.replies.push(Reply {
                        cref: pack_cref(ri, slot, c.gen),
                        arena,
                        off,
                        len: resp_len,
                        db: is_db,
                    });
                    c.flags |= CF_REPLIED;
                    if close {
                        c.flags |= CF_CLOSE_AFTER;
                    }
                    buf_ptr = buf_ptr.add(consumed);
                    n -= consumed;
                    break;
                }
            }
        }

        let c = &mut *self.rings[ri].conns.add(slot as usize);
        Self::stash(c, buf_ptr, n);
    }

    unsafe fn on_accept(&mut self, ri: usize, cqe: IoUringCqe) {
        let rs = &mut self.rings[ri];
        if (cqe.flags & IORING_CQE_F_MORE) == 0 {
            let bit = 1u8 << (ud_slot(cqe.user_data) as u8);
            rs.accept_armed &= !bit;
            rs.accept_cancelling &= !bit;
        }
        if cqe.res < 0 {
            return;
        }
        let slot = cqe.res as u32;
        if slot >= rs.r.nslots {
            return;
        }
        let c = &mut *rs.conns.add(slot as usize);
        if (c.flags & CF_OPEN) != 0 {
            return;
        }
        c.flags = CF_OPEN;
        c.ring = ri as u16;
        c.slot = slot;
        c.last_active = self.now_s;
        c.pend_len = 0;
        c.send_arena = ptr::null_mut();
        rs.open += 1;
        self.arm_poll(ri, slot);
    }

    unsafe fn on_poll(&mut self, ri: usize, cqe: IoUringCqe) {
        let slot = ud_slot(cqe.user_data);
        let gen = ud_gen(cqe.user_data);
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        if c.gen != gen || (c.flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN {
            return;
        }
        if (cqe.flags & IORING_CQE_F_MORE) == 0 {
            c.flags &= !CF_POLLING;
        }
        if cqe.res < 0 {
            self.conn_close(ri, slot);
            return;
        }
        let revents = cqe.res as i16;
        // Fast-path disconnect teardown on idle connections: if peer sent FIN/RST without unread
        // request bytes (or during a mass disconnect storm), close immediately without consuming a
        // receive buffer.
        if (revents & (libc::POLLHUP | libc::POLLERR)) != 0
            || ((revents & libc::POLLRDHUP) != 0
                && ((revents & libc::POLLIN) == 0 || (self.rq.len() - self.rq_head) >= 64))
        {
            if c.pend_len == 0 && (c.flags & (CF_SENDING | CF_REPLIED)) == 0 {
                self.conn_close(ri, slot);
                return;
            }
        }
        self.queue_read(ri, slot);
        if (cqe.flags & IORING_CQE_F_MORE) == 0 {
            let c = &mut *self.rings[ri].conns.add(slot as usize);
            if (c.flags & (CF_OPEN | CF_CLOSING | CF_REARM)) == CF_OPEN {
                c.flags |= CF_REARM;
                self.rings[ri].rearm.push(slot);
            }
        }
    }

    unsafe fn on_recv(&mut self, ri: usize, cqe: IoUringCqe) {
        let slot = ud_slot(cqe.user_data);
        let gen = ud_gen(cqe.user_data);
        let has_buf = (cqe.flags & IORING_CQE_F_BUFFER) != 0;
        let bid = (cqe.flags >> IORING_CQE_BUFFER_SHIFT) as u16;
        self.rings[ri].reads -= 1;

        let c = &mut *self.rings[ri].conns.add(slot as usize);
        let live = c.gen == gen && (c.flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN;
        if live {
            c.flags &= !CF_RECVING;
        }
        if live && cqe.res > 0 {
            c.last_active = self.now_s;
            let ptr = self.rings[ri].r.buf_ptr(bid);
            let n = cqe.res as usize;
            if self.answers_left > 0 {
                self.answers_left -= 1;
                self.conn_feed(ri, slot, ptr, n);
            } else {
                let c = &mut *self.rings[ri].conns.add(slot as usize);
                let slice = std::slice::from_raw_parts(ptr, n);
                Self::pend_append(c, slice);
                if (c.flags & CF_READY) == 0 {
                    c.flags |= CF_READY;
                    self.ready.push(pack_cref(ri, slot, c.gen));
                }
            }
        }
        if has_buf {
            self.rings[ri].r.buf_return(bid);
        }
        if !live {
            return;
        }
        if cqe.res > 0 {
            if (cqe.res as u32) == self.rings[ri].r.buf_size {
                self.queue_read(ri, slot);
            }
            return;
        }
        if cqe.res == -libc::EAGAIN || cqe.res == -libc::ENOBUFS {
            if cqe.res == -libc::ENOBUFS {
                self.queue_read(ri, slot);
            }
            return;
        }
        self.conn_close(ri, slot);
    }

    unsafe fn on_send(&mut self, ri: usize, cqe: IoUringCqe) {
        let slot = ud_slot(cqe.user_data);
        let gen = ud_gen(cqe.user_data);
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        if c.gen != gen {
            return;
        }
        c.flags &= !CF_SENDING;
        let sa = c.send_arena;
        c.send_arena = ptr::null_mut();
        if !sa.is_null() {
            self.arena_unref(sa);
        }
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        if cqe.res < 0 || (c.flags & CF_CLOSE_AFTER) != 0 {
            self.conn_close(ri, slot);
            return;
        }
        if c.pend_len != 0 && (c.flags & (CF_READY | CF_HELD | CF_CLOSING)) == 0 {
            c.flags |= CF_READY;
            self.ready.push(pack_cref(ri, slot, c.gen));
        }
    }

    unsafe fn on_close(&mut self, ri: usize, cqe: IoUringCqe) {
        let slot = ud_slot(cqe.user_data);
        let gen = ud_gen(cqe.user_data);
        let c = &mut *self.rings[ri].conns.add(slot as usize);
        if c.gen != gen || (c.flags & CF_CLOSING) == 0 {
            return;
        }
        self.conn_freed(ri, slot);
    }

    unsafe fn on_evfd(&mut self, cqe: IoUringCqe) {
        let ri = ud_ring(cqe.user_data);
        if (cqe.flags & IORING_CQE_F_MORE) == 0 {
            let efd = self.rings[ri].efd;
            let sqe = self.rings[0].r.get_sqe();
            (*sqe).opcode = IORING_OP_POLL_ADD;
            (*sqe).fd = efd;
            (*sqe).len = IORING_POLL_ADD_MULTI;
            (*sqe).op_flags = libc::POLLIN as u32;
            (*sqe).user_data = pack_ud(UD_EVFD, ri, 0, 0);
        }
    }

    unsafe fn reap(&mut self, ri: usize) -> u32 {
        let cq_head_ptr = self.rings[ri].r.cq_head;
        let cq_tail_ptr = self.rings[ri].r.cq_tail;
        let cq_mask = self.rings[ri].r.cq_mask;
        let cqes_ptr = self.rings[ri].r.cqes;

        let mut head = *cq_head_ptr;
        let tail = std::sync::atomic::AtomicU32::from_ptr(cq_tail_ptr as *mut u32)
            .load(std::sync::atomic::Ordering::Acquire);
        let mut n = 0u32;

        while head != tail {
            let cqe = *cqes_ptr.add((head & cq_mask) as usize);
            match ud_op(cqe.user_data) {
                UD_RECV => self.on_recv(ri, cqe),
                UD_POLL => self.on_poll(ri, cqe),
                UD_SEND => self.on_send(ri, cqe),
                UD_ACCEPT => self.on_accept(ri, cqe),
                UD_CLOSE => self.on_close(ri, cqe),
                UD_EVFD => self.on_evfd(cqe),
                _ => {}
            }
            head = head.wrapping_add(1);
            n += 1;
            if (n & 255) == 0 {
                std::sync::atomic::AtomicU32::from_ptr(cq_head_ptr)
                    .store(head, std::sync::atomic::Ordering::Release);
                self.rings[ri].r.buf_publish();
            }
        }
        std::sync::atomic::AtomicU32::from_ptr(cq_head_ptr)
            .store(head, std::sync::atomic::Ordering::Release);
        n
    }

    unsafe fn run_lists(&mut self) {
        let n = self.ready.len();
        let mut i = 0usize;
        while i < n && self.answers_left > 0 {
            let cref = self.ready[i];
            i += 1;
            let ri = cref_ring(cref);
            let slot = cref_slot(cref) as u32;
            let gen = cref_gen(cref);
            let c = &mut *self.rings[ri].conns.add(slot as usize);
            if c.gen != gen {
                continue;
            }
            c.flags &= !CF_READY;
            if (c.flags & CF_OPEN) == 0 || (c.flags & (CF_CLOSING | CF_SENDING | CF_HELD)) != 0 {
                continue;
            }
            self.answers_left -= 1;
            self.conn_feed(ri, slot, ptr::null(), 0);
        }
        if i > 0 {
            self.ready.copy_within(i.., 0);
            let new_len = self.ready.len() - i;
            self.ready.truncate(new_len);
        }

        if self.writes_ok && !self.held.is_empty() {
            let held_count = self.held.len();
            for k in 0..held_count {
                let cref = self.held[k];
                let ri = cref_ring(cref);
                let slot = cref_slot(cref) as u32;
                let gen = cref_gen(cref);
                let c = &mut *self.rings[ri].conns.add(slot as usize);
                if c.gen != gen {
                    continue;
                }
                c.flags &= !CF_HELD;
                if (c.flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN {
                    continue;
                }
                self.conn_feed(ri, slot, ptr::null(), 0);
            }
            self.held.copy_within(held_count.., 0);
            let rem = self.held.len() - held_count;
            self.held.truncate(rem);
        }
    }

    unsafe fn flush_replies(&mut self, commit_failed: bool) {
        for i in 0..self.replies.len() {
            let rp = self.replies[i];
            let ri = cref_ring(rp.cref);
            let slot = cref_slot(rp.cref) as u32;
            let gen = cref_gen(rp.cref);
            let c = &mut *self.rings[ri].conns.add(slot as usize);
            if c.gen != gen {
                continue;
            }
            c.flags &= !CF_REPLIED;
            if (c.flags & (CF_OPEN | CF_CLOSING)) != CF_OPEN {
                continue;
            }

            let (mut a, mut ptr, mut len) = (
                rp.arena,
                Arena::data_ptr(rp.arena).add(rp.off as usize),
                rp.len,
            );
            if commit_failed && rp.db {
                let p500 = self.arena_reserve(R_500.len());
                ptr::copy_nonoverlapping(R_500.as_ptr(), p500, R_500.len());
                (*self.cur_arena).used += R_500.len() as u32;
                a = self.cur_arena;
                ptr = p500;
                len = R_500.len() as u32;
            }

            let rs = &mut self.rings[ri];
            let sqe = rs.r.get_sqe();
            (*sqe).opcode = IORING_OP_SEND;
            (*sqe).flags = IOSQE_FIXED_FILE;
            (*sqe).fd = slot as i32;
            (*sqe).addr = ptr as u64;
            (*sqe).len = len;
            (*sqe).op_flags = (libc::MSG_NOSIGNAL | libc::MSG_WAITALL) as u32;
            (*sqe).user_data = pack_ud(UD_SEND, ri, slot, gen);

            (*a).refs += 1;
            let c = &mut *rs.conns.add(slot as usize);
            c.send_arena = a;
            c.flags |= CF_SENDING;
        }
        self.replies.clear();
        for i in 0..self.batch_arenas.len() {
            let a = self.batch_arenas[i];
            self.arena_unref(a);
        }
        self.batch_arenas.clear();
        self.cur_arena = ptr::null_mut();
    }

    unsafe fn sweep(&mut self) {
        let nr = self.rings.len();
        for ri in 0..nr {
            if self.rings[ri].open == 0 {
                continue;
            }
            let nslots = self.rings[ri].r.nslots;
            for slot in 0..nslots {
                let c = &*self.rings[ri].conns.add(slot as usize);
                if (c.flags & (CF_OPEN | CF_CLOSING)) == CF_OPEN
                    && self.now_s.wrapping_sub(c.last_active) > IDLE_SECONDS
                {
                    self.conn_close(ri, slot);
                }
            }
        }
    }

    pub fn run(&mut self) {
        unsafe {
            let mut last_sweep = now_monotonic_ns();
            let mut last_cqes = 0u32;
            loop {
                if crate::train::TRAINING_DONE.load(std::sync::atomic::Ordering::Acquire) {
                    astraea_eng_close(self.eng);
                    return;
                }
                for i in 1..self.rings.len() {
                    if self.rings[i].r.pending_submit() > 0 {
                        let _ = self.rings[i].r.submit_raw(0, 0, ptr::null(), 0);
                    }
                }

                let has_queued_work = !self.ready.is_empty()
                    || self.rq_head < self.rq.len()
                    || (self.writes_ok && !self.held.is_empty());
                let min_wait = if has_queued_work {
                    0
                } else if last_cqes >= 8 {
                    BATCH_WAIT
                } else {
                    1
                };
                let wait_ns = if last_cqes >= 8 || has_queued_work {
                    BATCH_WAIT_NS
                } else {
                    1_000_000_000
                };

                let rc = self.rings[0].r.wait(min_wait, wait_ns);
                if rc < 0
                    && rc != -libc::ETIME
                    && rc != -libc::EINTR
                    && rc != -libc::EBUSY
                    && rc != -libc::EAGAIN
                {
                    std::process::exit(1);
                }

                let t = now_monotonic_ns();
                self.now_s = (t / 1_000_000_000) as u32;
                self.unix_now_s = now_realtime_secs();
                self.writes_ok = astraea_eng_writes_paused(self.eng) == 0;
                self.answers_left = ANSWERS_PER_BATCH;

                let mut ncq = self.reap(0);
                for i in 1..self.rings.len() {
                    ncq += self.reap(i);
                }

                if self.issue_reads() > 0 {
                    for i in 0..self.rings.len() {
                        if self.rings[i].r.pending_submit() > 0 {
                            let _ = self.rings[i].r.submit_raw(0, 0, ptr::null(), 0);
                        }
                    }
                    for i in 0..self.rings.len() {
                        ncq += self.reap(i);
                    }
                }

                self.run_lists();

                let mut commit_failed = false;
                if self.batch_tx_open {
                    commit_failed = astraea_eng_commit_batch(self.eng) < 0;
                    self.batch_tx_open = false;
                }
                astraea_eng_after_batch(self.eng);

                self.flush_replies(commit_failed);

                for ri in 0..self.rings.len() {
                    self.rings[ri].r.buf_publish();
                    let nrearm = self.rings[ri].rearm.len();
                    for k in 0..nrearm {
                        let slot = self.rings[ri].rearm[k];
                        let c = &*self.rings[ri].conns.add(slot as usize);
                        if (c.flags & (CF_OPEN | CF_CLOSING | CF_REARM)) == (CF_OPEN | CF_REARM) {
                            self.arm_poll(ri, slot);
                        }
                    }
                    self.rings[ri].rearm.clear();
                }

                self.accept_policy();
                last_cqes = ncq;

                if t.wrapping_sub(last_sweep) > SWEEP_NS {
                    self.sweep();
                    last_sweep = t;
                }
            }
        }
    }
}
