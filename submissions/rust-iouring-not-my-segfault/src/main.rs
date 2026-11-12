mod auth;
mod conn;
mod http;
mod json;
mod ring;
mod server;
mod stats;
mod store;
mod wal;

#[cfg(test)]
mod tests;

use server::ListenerLinger;
use settings::{Settings, parse_args};
use std::env;
use std::fmt;
use std::io::{self, Write};
use std::mem::MaybeUninit;
use std::net::{SocketAddr, TcpListener};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd};
use std::time::Instant;

// The safe build runs no listener-linger check and sets no socket option; both
// branches of `if SAFE_BUILD` compile in every build.
const SAFE_BUILD: bool = cfg!(twelve_safe);

mod settings {
    use std::env;
    use std::net::IpAddr;

    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    pub(crate) enum RingSpread {
        Fill,
        Spread,
    }

    impl RingSpread {
        pub(crate) fn as_str(self) -> &'static str {
            match self {
                Self::Fill => "fill",
                Self::Spread => "spread",
            }
        }
    }

    // Validated command-line flags and SPEC.md's environment values; parse_args is the only constructor.
    pub(crate) struct Settings {
        sqlite_path: String,
        jwt_secret: String,
        host: IpAddr,
        port: u16,
        batch_commit: bool,
        check_reads: bool,
        wal_gate: u32,
        cache_kib: u32,
        mmap_mib: u32,
        wake_batch: u32,
        wake_wait_us: u32,
        turn_cap: u32,
        effective_turn_cap: u32,
        effective_wake_batch: u32,
        poll_recv: bool,
        ckpt_restart: bool,
        batch_target: u32,
        ring_spread: RingSpread,
    }

    impl Settings {
        pub(crate) fn sqlite_path(&self) -> &str {
            &self.sqlite_path
        }
        pub(crate) fn jwt_secret(&self) -> &str {
            &self.jwt_secret
        }
        pub(crate) fn host(&self) -> IpAddr {
            self.host
        }
        pub(crate) fn port(&self) -> u16 {
            self.port
        }
        pub(crate) fn batch_commit(&self) -> bool {
            self.batch_commit
        }
        pub(crate) fn check_reads(&self) -> bool {
            self.check_reads
        }
        pub(crate) fn wal_gate(&self) -> u32 {
            self.wal_gate
        }
        pub(crate) fn cache_kib(&self) -> u32 {
            self.cache_kib
        }
        pub(crate) fn mmap_mib(&self) -> u32 {
            self.mmap_mib
        }
        pub(crate) fn wake_batch(&self) -> u32 {
            self.wake_batch
        }
        pub(crate) fn wake_wait_us(&self) -> u32 {
            self.wake_wait_us
        }
        pub(crate) fn turn_cap(&self) -> u32 {
            self.turn_cap
        }
        pub(crate) fn effective_turn_cap(&self) -> u32 {
            self.effective_turn_cap
        }
        pub(crate) fn effective_wake_batch(&self) -> u32 {
            self.effective_wake_batch
        }
        pub(crate) fn poll_recv(&self) -> bool {
            self.poll_recv
        }
        pub(crate) fn ckpt_restart(&self) -> bool {
            self.ckpt_restart
        }
        pub(crate) fn batch_target(&self) -> u32 {
            self.batch_target
        }
        pub(crate) fn ring_spread(&self) -> RingSpread {
            self.ring_spread
        }
    }

    fn decimal(text: &str, min: u32, max: u32) -> Option<u32> {
        if text.is_empty() {
            return None;
        }
        let mut value = 0_u32;
        for byte in text.bytes() {
            if !byte.is_ascii_digit() {
                return None;
            }
            value = value.checked_mul(10)?.checked_add(u32::from(byte - b'0'))?;
        }
        (min..=max).contains(&value).then_some(value)
    }

    fn variable(name: &str) -> Result<String, String> {
        env::var(name).map_err(|error| match error {
            env::VarError::NotPresent => format!("missing {name}"),
            env::VarError::NotUnicode(_) => format!("invalid {name}"),
        })
    }

    /// # Errors
    /// Returns the first fixed configuration refusal without opening resources.
    pub(crate) fn parse_args(args: impl Iterator<Item = String>) -> Result<Settings, String> {
        let mut batch_commit = false;
        let mut check_reads = false;
        let mut poll_recv = false;
        let mut ckpt_restart = false;
        // The gate is the backstop: tail holds restart the WAL at about 5,000 frames, and only a disk too
        // slow for them lets it reach 32,768 frames (128 MiB), where writes wait without a time limit.
        let mut wal_gate = 32_768;
        let mut cache_kib = 32_768;
        let mut mmap_mib = 0;
        let mut wake_batch = 128;
        let mut wake_wait_us = 2_000;
        let mut turn_cap = 1_024;
        let mut batch_target = 64;
        let mut ring_spread = RingSpread::Fill;
        let mut args = args;
        while let Some(arg) = args.next() {
            if arg == "--batch-target" || arg.starts_with("--batch-target=") {
                let text = match arg.split_once('=') {
                    Some((_, text)) => text.to_owned(),
                    None => args.next().ok_or("invalid --batch-target")?,
                };
                batch_target = decimal(&text, 1, 1_024).ok_or("invalid --batch-target")?;
                continue;
            }
            if arg == "--ring-spread" || arg.starts_with("--ring-spread=") {
                let text = match arg.split_once('=') {
                    Some((_, text)) => text.to_owned(),
                    None => args.next().ok_or("invalid --ring-spread")?,
                };
                ring_spread = match text.as_str() {
                    "fill" => RingSpread::Fill,
                    "spread" => RingSpread::Spread,
                    _ => return Err("invalid --ring-spread".into()),
                };
                continue;
            }
            match arg.as_str() {
                "--batch-commit" => batch_commit = true,
                "--check-reads" => check_reads = true,
                "--poll-recv" => poll_recv = true,
                "--ckpt-restart" => ckpt_restart = true,
                _ => {
                    let Some((name, text)) = arg.split_once('=') else {
                        return Err("unknown argument".into());
                    };
                    let (slot, min, max) = match name {
                        "--wal-gate" => (&mut wal_gate, 1_024, 65_536),
                        "--cache-kib" => (&mut cache_kib, 1_000, 262_144),
                        "--mmap-mib" => (&mut mmap_mib, 0, 1_024),
                        "--wake-batch" => (&mut wake_batch, 1, 1_024),
                        "--wake-wait-us" => (&mut wake_wait_us, 0, 10_000),
                        "--turn-cap" => (&mut turn_cap, 64, 131_072),
                        _ => return Err("unknown argument".into()),
                    };
                    *slot = decimal(text, min, max).ok_or_else(|| format!("invalid {name}"))?;
                }
            }
        }
        if wake_batch > turn_cap {
            return Err("--wake-batch exceeds --turn-cap".into());
        }
        let sqlite_path = variable("SQLITE_PATH")?;
        if sqlite_path.is_empty() || sqlite_path.contains('\0') {
            return Err("invalid SQLITE_PATH".into());
        }
        let jwt_secret = variable("JWT_SECRET")?;
        if jwt_secret.is_empty() {
            return Err("invalid JWT_SECRET".into());
        }
        let host = variable("HOST")?
            .parse()
            .map_err(|_| String::from("invalid HOST"))?;
        let port = decimal(&variable("PORT")?, 1, 65_535)
            .and_then(|value| u16::try_from(value).ok())
            .ok_or_else(|| String::from("invalid PORT"))?;
        let effective_turn_cap = if cfg!(twelve_small_limits) {
            16
        } else {
            turn_cap
        };
        let effective_wake_batch = if cfg!(twelve_small_limits) {
            wake_batch.min(16)
        } else {
            wake_batch
        };
        Ok(Settings {
            sqlite_path,
            jwt_secret,
            host,
            port,
            batch_commit,
            check_reads,
            wal_gate,
            cache_kib,
            mmap_mib,
            wake_batch,
            wake_wait_us,
            turn_cap,
            effective_turn_cap,
            effective_wake_batch,
            poll_recv,
            ckpt_restart,
            batch_target,
            ring_spread,
        })
    }
}

struct StartupError {
    stage: &'static str,
    detail: String,
}

impl StartupError {
    fn new(stage: &'static str, detail: impl ToString) -> Self {
        Self {
            stage,
            detail: detail.to_string(),
        }
    }
}

fn file_limit() -> Result<u32, StartupError> {
    let mut limit = MaybeUninit::<libc::rlimit>::uninit();
    // SAFETY: getrlimit initializes this writable rlimit on success. The value is
    // read only after its checked successful return.
    if unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, limit.as_mut_ptr()) } != 0 {
        return Err(StartupError::new("file-limit", "getrlimit failed"));
    }
    // SAFETY: the preceding successful getrlimit initialized both fields.
    let mut limit = unsafe { limit.assume_init() };
    if limit.rlim_cur != limit.rlim_max {
        limit.rlim_cur = limit.rlim_max;
        // SAFETY: the valid rlimit preserves the existing hard value; only this
        // process's soft descriptor limit is raised to that same permitted value.
        if unsafe { libc::setrlimit(libc::RLIMIT_NOFILE, &limit) } != 0 {
            return Err(StartupError::new("file-limit", "setrlimit failed"));
        }
    }
    let slots = if limit.rlim_max == libc::RLIM_INFINITY {
        65_535
    } else {
        u32::try_from(limit.rlim_max.min(65_535))
            .map_err(|_| StartupError::new("file-limit", "hard limit below 16"))?
    };
    if slots < 16 {
        return Err(StartupError::new("file-limit", "hard limit below 16"));
    }
    Ok(slots)
}

fn kernel_release() -> Result<String, StartupError> {
    let mut name = MaybeUninit::<libc::utsname>::uninit();
    // SAFETY: uname receives a writable correctly aligned utsname and initializes
    // it on success; no field is read before its return is checked.
    if unsafe { libc::uname(name.as_mut_ptr()) } != 0 {
        return Err(StartupError::new("kernel", "uname failed"));
    }
    // SAFETY: uname succeeded and initialized the entire utsname.
    let name = unsafe { name.assume_init() };
    let end = name
        .release
        .iter()
        .position(|byte| *byte == 0)
        .ok_or_else(|| StartupError::new("kernel", "invalid release"))?;
    // c_char's signedness does not change the bytes of the native character field.
    let bytes: Vec<u8> = name
        .release
        .get(..end)
        .expect("position found above")
        .iter()
        .map(|byte| byte.cast_unsigned())
        .collect();
    String::from_utf8(bytes).map_err(|_| StartupError::new("kernel", "invalid release"))
}

struct JsonString<'a>(&'a str);

impl fmt::Display for JsonString<'_> {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        out.write_str("\"")?;
        for scalar in self.0.chars() {
            match scalar {
                '"' => out.write_str("\\\"")?,
                '\\' => out.write_str("\\\\")?,
                '\0'..='\u{001f}' => write!(out, "\\u{:04x}", u32::from(scalar))?,
                _ => write!(out, "{scalar}")?,
            }
        }
        out.write_str("\"")
    }
}

fn startup_line(
    settings: &Settings,
    startup: ring::Startup,
    readers: bool,
    kernel: &str,
    gate: u32,
    listener_linger: ListenerLinger,
) -> io::Result<()> {
    let mut out = io::stderr().lock();
    write!(out, "startup kernel={} features=[", JsonString(kernel))?;
    for (index, name) in ring::PROBED.iter().enumerate() {
        if index != 0 {
            write!(out, ",")?;
        }
        write!(out, "{}", JsonString(name))?;
    }
    write!(
        out,
        "] pbuf_retry={} rings={} slots_per_ring={} bufs_per_ring={}",
        u8::from(startup.pbuf_retry),
        startup.rings,
        startup.slots_per_ring,
        startup.bufs_per_ring
    )?;
    write!(
        out,
        " batch_commit={} check_reads={} wal_gate={} cache_kib={} mmap_mib={} wake_batch={} wake_wait_us={} turn_cap={} poll_recv={} ckpt_restart={}",
        u8::from(settings.batch_commit()),
        u8::from(settings.check_reads()),
        settings.wal_gate(),
        settings.cache_kib(),
        settings.mmap_mib(),
        settings.wake_batch(),
        settings.wake_wait_us(),
        settings.turn_cap(),
        u8::from(settings.poll_recv()),
        u8::from(settings.ckpt_restart())
    )?;
    write!(
        out,
        " effective_wal_gate={} effective_wake_batch={} effective_turn_cap={} sqlite_source_id={} readers_opened={} source_sha256={} rustflags={} c_target={} pgo={}",
        gate,
        settings.effective_wake_batch(),
        settings.effective_turn_cap(),
        JsonString(env!("TWELVE_SQLITE_SOURCE_ID")),
        u8::from(readers),
        env!("TWELVE_SOURCE_SHA256"),
        env!("TWELVE_RUSTFLAGS"),
        JsonString(env!("TWELVE_C_TARGET")),
        env!("TWELVE_PGO")
    )?;
    write!(
        out,
        " listener_linger={} close_trickle={} close_bulk_ring={}",
        match listener_linger {
            ListenerLinger::Inherited => "inherited",
            ListenerLinger::PerClose => "per_close",
        },
        server::CLOSE_TRICKLE,
        server::CLOSE_BULK_RING
    )?;
    writeln!(
        out,
        " safe_build={} small_limits={} fault_inject={} host=\"{}\" port={} backlog=4096 batch_target={} ring_spread={}",
        u8::from(cfg!(twelve_safe)),
        u8::from(cfg!(twelve_small_limits)),
        u8::from(cfg!(twelve_fault_inject)),
        settings.host(),
        settings.port(),
        settings.batch_target(),
        settings.ring_spread().as_str()
    )
}

fn compose(settings: &Settings) -> Result<server::Server, StartupError> {
    let slots = file_limit()?;
    let mut store = store::Store::open(
        settings.sqlite_path(),
        &store::StoreSettings {
            check_reads: settings.check_reads(),
            cache_kib: settings.cache_kib(),
            mmap_mib: settings.mmap_mib(),
        },
    )
    .map_err(|error| StartupError::new("store", error))?;
    store
        .health()
        .map_err(|error| StartupError::new("health", error))?;
    let gate = if cfg!(twelve_small_limits) {
        64
    } else {
        settings.wal_gate()
    };
    let wal = wal::Wal::open(
        settings.sqlite_path(),
        &wal::WalSettings {
            gate_frames: gate,
            ckpt_restart: settings.ckpt_restart(),
        },
    )
    .map_err(|error| StartupError::new("wal", error))?;
    let buffers = if cfg!(twelve_small_limits) { 16 } else { 1_024 };
    let rings =
        ring::Rings::new(slots, buffers).map_err(|error| StartupError::new("rings", error))?;
    let listener = TcpListener::bind(SocketAddr::new(settings.host(), settings.port()))
        .map_err(|error| StartupError::new("bind", error))?;
    // SAFETY: listener owns this live listening socket; listen changes its backlog
    // only, and the same owner remains alive until Server takes it by value.
    if unsafe { libc::listen(listener.as_raw_fd(), 4_096) } != 0 {
        return Err(StartupError::new("listen", io::Error::last_os_error()));
    }
    let listener_linger = listener_linger(&listener);
    let conns = conn::Conns::new(slots);
    let auth = auth::Auth::new(settings.jwt_secret().as_bytes());
    let stats = stats::Stats::new(Instant::now());
    let startup = rings.startup();
    let readers = store.readers_opened();
    let kernel = kernel_release()?;
    let progress = stats::start_watchdog();
    let server = server::Server::new(
        settings,
        listener,
        rings,
        conns,
        store,
        wal,
        auth,
        stats,
        progress,
        listener_linger,
    );
    if startup_line(settings, startup, readers, &kernel, gate, listener_linger).is_err() {
        std::process::exit(1);
    }
    nonblocking_stderr();
    Ok(server)
}

// From here on no stderr write may stall the loop or the watchdog: stderr becomes nonblocking, so
// a full pipe or socket refuses a write at once (EAGAIN), and every later line, the panic hook's
// included, goes out in one write or is dropped and counted (stats::stderr_line).
fn nonblocking_stderr() {
    let stderr = io::stderr();
    let fd = stderr.as_fd();
    // SAFETY: fcntl with F_GETFL reads the status flags of the borrowed, open descriptor and touches
    // no memory.
    let flags = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_GETFL) };
    // F_GETFL fails only for a closed stderr, which no write can block on; F_SETFL keeps every
    // other flag, so it too can fail only for a closed descriptor.
    if flags >= 0 && flags & libc::O_NONBLOCK == 0 {
        // SAFETY: fcntl with F_SETFL sets the borrowed descriptor's status flags from an integer and
        // touches no memory.
        let _ = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFL, flags | libc::O_NONBLOCK) };
    }
}

// Whether accepted sockets start with the listener's zero linger: the fast build checks once that
// an accepted socket inherits it, then sets it on the real listener; the safe build sets nothing.
fn listener_linger(listener: &TcpListener) -> ListenerLinger {
    if SAFE_BUILD || !linger_inherited() {
        return ListenerLinger::PerClose;
    }
    // UNSAFE PATH (speed): no orderly close (half-close, then read to the peer's end of stream); accepted sockets inherit this zero linger, so a close with nothing unacknowledged resets, server.rs turns linger off before every other close, and the contest's hang-up budget was measured with this close
    if set_linger0(listener.as_fd()).is_ok() {
        ListenerLinger::Inherited
    } else {
        ListenerLinger::PerClose
    }
}

// Checks once, on a loopback pair, that an accepted socket inherits its listener's SO_LINGER
// {1, 0}; any failed step fails the check.
fn linger_inherited() -> bool {
    let Ok(probe) = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)) else {
        return false;
    };
    if set_linger0(probe.as_fd()).is_err() {
        return false;
    }
    let Ok(address) = probe.local_addr() else {
        return false;
    };
    let Ok(client) = std::net::TcpStream::connect(address) else {
        return false;
    };
    let Ok((accepted, _)) = probe.accept() else {
        return false;
    };
    let mut value = libc::linger {
        l_onoff: 0,
        l_linger: -1,
    };
    let mut len = libc::socklen_t::try_from(size_of::<libc::linger>()).expect("linger size");
    // SAFETY: getsockopt needs a live socket and a writable buffer of `len` bytes: `accepted`
    // owns the socket for this call, `value` is a live libc::linger and `len` is its size.
    let done = unsafe {
        libc::getsockopt(
            accepted.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_LINGER,
            (&raw mut value).cast(),
            &mut len,
        )
    };
    drop(client);
    drop(accepted);
    done == 0 && value.l_onoff == 1 && value.l_linger == 0
}

fn set_linger0(fd: BorrowedFd<'_>) -> io::Result<()> {
    let value = libc::linger {
        l_onoff: 1,
        l_linger: 0,
    };
    // SAFETY: setsockopt needs a live socket and a readable value of the given length: `fd` is
    // borrowed for this call, `value` is a live libc::linger of size_of::<libc::linger>() bytes,
    // and the kernel copies it before returning.
    let done = unsafe {
        libc::setsockopt(
            fd.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_LINGER,
            (&raw const value).cast(),
            libc::socklen_t::try_from(size_of::<libc::linger>()).expect("linger size"),
        )
    };
    if done != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

// Writes one panic line: `panic: `, the text with every CR and LF replaced by a space, then LF.
fn panic_line(out: &mut dyn Write, text: &dyn fmt::Display) -> io::Result<()> {
    out.write_all(b"panic: ")?;
    let mut line = OneLine {
        out: &mut *out,
        error: None,
    };
    if fmt::write(&mut line, format_args!("{text}")).is_err() {
        return Err(line
            .error
            .unwrap_or_else(|| io::Error::other("panic text formatting failed")));
    }
    out.write_all(b"\n")
}

// A fmt::Write adapter that replaces each CR and LF with a space and keeps the first I/O error.
struct OneLine<'a> {
    out: &'a mut dyn Write,
    error: Option<io::Error>,
}

impl fmt::Write for OneLine<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        for (index, run) in text.split(['\r', '\n']).enumerate() {
            if index != 0 {
                self.put(b" ")?;
            }
            self.put(run.as_bytes())?;
        }
        Ok(())
    }
}

impl OneLine<'_> {
    fn put(&mut self, bytes: &[u8]) -> fmt::Result {
        self.out.write_all(bytes).map_err(|error| {
            self.error = Some(error);
            fmt::Error
        })
    }
}

fn refusal(prefix: &str, detail: &str, status: i32) -> ! {
    let mut out = io::stderr().lock();
    let result = (|| -> io::Result<()> {
        write!(out, "{prefix}")?;
        for scalar in detail.chars() {
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
    })();
    std::process::exit(if result.is_ok() { status } else { 1 });
}

fn main() {
    std::panic::set_hook(Box::new(|info| {
        // One write, whole or not at all (stats::stderr_line). The result is ignored: the process
        // is already panicking and has no other report path.
        let _ = stats::stderr_line(|out| panic_line(out, info));
    }));
    let args = env::args_os().skip(1).map(|arg| {
        arg.into_string()
            .unwrap_or_else(|_| refusal("startup refused: ", "argument is not UTF-8", 2))
    });
    let settings = parse_args(args).unwrap_or_else(|error| refusal("startup refused: ", &error, 2));
    let server = compose(&settings).unwrap_or_else(|error| {
        refusal(
            &format!("startup failed: {}: ", error.stage),
            &error.detail,
            1,
        )
    });
    drop(settings);
    server.run()
}
