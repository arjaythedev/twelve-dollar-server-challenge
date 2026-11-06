pub mod http;
pub mod jwt;
pub mod train;
pub mod uring;

#[allow(non_camel_case_types, dead_code)]
pub mod libc {
    pub type c_int = i32;
    pub type c_uint = u32;
    pub type c_long = i64;
    pub type c_ulong = u64;
    pub type c_void = std::ffi::c_void;
    pub type size_t = usize;
    pub type off_t = i64;

    pub const AF_INET: c_int = 2;
    pub const SOCK_STREAM: c_int = 1;
    pub const SOCK_NONBLOCK: c_int = 0o4000;
    pub const SOCK_CLOEXEC: c_int = 0o2000000;
    pub const SOL_SOCKET: c_int = 1;
    pub const SO_REUSEADDR: c_int = 2;
    pub const SO_REUSEPORT: c_int = 15;
    pub const IPPROTO_TCP: c_int = 6;
    pub const TCP_NODELAY: c_int = 1;

    pub const PROT_READ: c_int = 0x1;
    pub const PROT_WRITE: c_int = 0x2;
    pub const MAP_SHARED: c_int = 0x01;
    pub const MAP_PRIVATE: c_int = 0x02;
    pub const MAP_ANONYMOUS: c_int = 0x20;
    pub const MAP_POPULATE: c_int = 0x08000;
    pub const MAP_FAILED: *mut c_void = !0usize as *mut c_void;

    pub const EFD_NONBLOCK: c_int = 0o4000;
    pub const EFD_CLOEXEC: c_int = 0o2000000;

    pub const POLLIN: i16 = 0x0001;
    pub const POLLERR: i16 = 0x0008;
    pub const POLLHUP: i16 = 0x0010;
    pub const POLLRDHUP: i16 = 0x2000;

    pub const MSG_DONTWAIT: c_int = 0x40;
    pub const MSG_WAITALL: c_int = 0x100;
    pub const MSG_NOSIGNAL: c_int = 0x4000;

    pub const CLOCK_REALTIME: c_int = 0;
    pub const CLOCK_MONOTONIC: c_int = 1;

    pub const EINTR: c_int = 4;
    pub const EAGAIN: c_int = 11;
    pub const ENOMEM: c_int = 12;
    pub const EBUSY: c_int = 16;
    pub const EINVAL: c_int = 22;
    pub const ETIME: c_int = 62;
    pub const EADDRINUSE: c_int = 98;
    pub const ENOBUFS: c_int = 105;

    pub const SIGINT: c_int = 2;
    pub const SIGPIPE: c_int = 13;
    pub const SIGTERM: c_int = 15;
    pub const SIG_IGN: usize = 1;

    pub const RLIMIT_NOFILE: c_int = 7;

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct in_addr {
        pub s_addr: u32,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct sockaddr {
        pub sa_family: u16,
        pub sa_data: [u8; 14],
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct sockaddr_in {
        pub sin_family: u16,
        pub sin_port: u16,
        pub sin_addr: in_addr,
        pub sin_zero: [u8; 8],
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct timespec {
        pub tv_sec: i64,
        pub tv_nsec: i64,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct rlimit {
        pub rlim_cur: u64,
        pub rlim_max: u64,
    }

    unsafe extern "C" {
        pub fn syscall(num: c_long, ...) -> c_long;
        pub fn __errno_location() -> *mut c_int;
        pub fn mmap(
            addr: *mut c_void,
            len: size_t,
            prot: c_int,
            flags: c_int,
            fd: c_int,
            offset: off_t,
        ) -> *mut c_void;
        pub fn munmap(addr: *mut c_void, len: size_t) -> c_int;
        pub fn mlock(addr: *const c_void, len: size_t) -> c_int;
        pub fn close(fd: c_int) -> c_int;
        pub fn eventfd(initval: c_uint, flags: c_int) -> c_int;
        pub fn socket(domain: c_int, ty: c_int, protocol: c_int) -> c_int;
        pub fn setsockopt(
            socket: c_int,
            level: c_int,
            name: c_int,
            value: *const c_void,
            option_len: u32,
        ) -> c_int;
        pub fn bind(socket: c_int, address: *const sockaddr, address_len: u32) -> c_int;
        pub fn listen(socket: c_int, backlog: c_int) -> c_int;
        pub fn usleep(usec: u32) -> c_int;
        pub fn clock_gettime(clk_id: c_int, tp: *mut timespec) -> c_int;
        pub fn malloc(size: size_t) -> *mut c_void;
        pub fn realloc(p: *mut c_void, size: size_t) -> *mut c_void;
        pub fn free(p: *mut c_void);
        pub fn signal(signum: c_int, handler: usize) -> usize;
        pub fn _exit(status: c_int) -> !;
        pub fn getrlimit(resource: c_int, rlim: *mut rlimit) -> c_int;
        pub fn setrlimit(resource: c_int, rlim: *const rlimit) -> c_int;
    }
}

use std::ffi::CString;
use std::io::{BufRead, BufReader};

#[repr(C)]
pub struct astraea_eng {
    _private: [u8; 0],
}

unsafe extern "C" {
    pub fn astraea_eng_open(path: *const i8, cache_kib: i32) -> *mut astraea_eng;
    pub fn astraea_eng_close(e: *mut astraea_eng);
    pub fn astraea_eng_writes_paused(e: *mut astraea_eng) -> i32;
    pub fn astraea_eng_begin_batch(e: *mut astraea_eng) -> i32;
    pub fn astraea_eng_commit_batch(e: *mut astraea_eng) -> i32;
    pub fn astraea_eng_after_batch(e: *mut astraea_eng);
    pub fn astraea_eng_feed(
        e: *mut astraea_eng,
        out: *mut i8,
        cap: usize,
        out_len: *mut usize,
    ) -> i32;
    pub fn astraea_eng_get_post(
        e: *mut astraea_eng,
        id: i64,
        out: *mut i8,
        cap: usize,
        out_len: *mut usize,
    ) -> i32;
    pub fn astraea_eng_like(
        e: *mut astraea_eng,
        uid: i64,
        pid: i64,
        out: *mut i8,
        cap: usize,
        out_len: *mut usize,
    ) -> i32;
    pub fn astraea_eng_create(
        e: *mut astraea_eng,
        uid: i64,
        user: *const i8,
        user_len: usize,
        body: *const i8,
        body_len: usize,
        out: *mut i8,
        cap: usize,
        out_len: *mut usize,
    ) -> i32;
    pub fn astraea_eng_health(
        e: *mut astraea_eng,
        uptime_s: i64,
        out: *mut i8,
        cap: usize,
        out_len: *mut usize,
    ) -> i32;
    pub fn astraea_make_train_db(path: *const i8, users: i32, posts: i32, likes: i32) -> i32;
    pub fn astraea_gcov_flush();
}

extern "C" fn on_term(_sig: libc::c_int) {
    unsafe {
        libc::_exit(0);
    }
}

fn lock_exe_code() {
    let Ok(exe) = std::fs::read_link("/proc/self/exe") else {
        return;
    };
    let Some(exe_str) = exe.to_str() else {
        return;
    };
    let Ok(f) = std::fs::File::open("/proc/self/maps") else {
        return;
    };
    for line in BufReader::new(f).lines().map_while(Result::ok) {
        if !line.ends_with(exe_str) {
            continue;
        }
        let Some(range) = line.split_whitespace().next() else {
            continue;
        };
        let Some((lo_s, hi_s)) = range.split_once('-') else {
            continue;
        };
        if let (Ok(lo), Ok(hi)) = (
            usize::from_str_radix(lo_s, 16),
            usize::from_str_radix(hi_s, 16),
        ) {
            if hi > lo {
                unsafe {
                    let _ = libc::mlock(lo as *const libc::c_void, hi - lo);
                }
            }
        }
    }
}

#[inline(never)]
fn prefault_stack() {
    let mut stk = [0u8; 256 * 1024];
    let mut i = 0;
    while i < stk.len() {
        unsafe {
            std::ptr::write_volatile(&mut stk[i], 0);
        }
        i += 4096;
    }
    std::hint::black_box(&stk);
}

fn main() {
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        libc::signal(libc::SIGTERM, on_term as *const () as usize);
        libc::signal(libc::SIGINT, on_term as *const () as usize);
    }

    let mut nslots: u32 = 65472;
    unsafe {
        let mut rl = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl) == 0 {
            if rl.rlim_cur < rl.rlim_max {
                rl.rlim_cur = rl.rlim_max;
                let _ = libc::setrlimit(libc::RLIMIT_NOFILE, &rl);
                let _ = libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl);
            }
            let lim = rl.rlim_cur.min(65535) as u32;
            nslots = if lim > 256 { lim - 64 } else { lim };
        }
    }

    for arg in std::env::args().skip(1) {
        if arg == "--pgo-train" {
            train::run_pgo_training(nslots);
        }
    }

    let host = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into());
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(80);
    let db_path = std::env::var("SQLITE_PATH").expect("SQLITE_PATH required");
    let secret = std::env::var("JWT_SECRET").expect("JWT_SECRET required");
    let cache_kib: i32 = std::env::var("CACHE_KIB")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(4096);

    let want_conns: u32 = 500_000;
    let nr_max = ((want_conns + nslots - 1) / nslots).clamp(1, 8) as usize;

    let c_db = CString::new(db_path).expect("valid SQLITE_PATH");
    let eng = unsafe { astraea_eng_open(c_db.as_ptr(), cache_kib) };
    if eng.is_null() {
        eprintln!("failed to open SQLite database");
        std::process::exit(1);
    }

    lock_exe_code();
    prefault_stack();

    let mut srv =
        uring::Server::new(&host, port, eng, secret.as_bytes(), nslots, nr_max).expect("server");
    srv.run();
}
