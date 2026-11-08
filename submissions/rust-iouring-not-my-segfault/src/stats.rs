use std::fmt;
use std::io::{self, Write};
use std::sync::atomic::{AtomicU8, AtomicU64, Ordering};
use std::sync::{Arc, Weak};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// Regular lines come ten seconds apart.
const EMIT_PERIOD: Duration = Duration::from_secs(10);
// Closure starts since the last written line that open a wind-down episode: the default turn cap,
// one full turn of closures; fewer, about 102 a second between two regular lines, open none.
const WIND_DOWN_CLOSES: u64 = 1024;
// One write of at most PIPE_BUF (4,096) bytes reaches a pipe whole or not at all (pipe(7)). The
// longest stats line is 3,221 bytes: "stats t=" with a 20-character t, then 96 keys with 20-digit
// values.
const LINE_MAX: usize = 4_096;

// Lines stderr could not take at once, which stderr_line dropped: counted in memory only, since
// the stats line takes no field for them. Owned by this module; every thread that writes stderr
// adds to it.
static DROPPED_LINES: AtomicU64 = AtomicU64::new(0);

macro_rules! counter_type {
    (count) => { u64 };
    (gauge) => { u64 };
    (max) => { u64 };
    (min) => { Option<u64> };
}

macro_rules! reset_extremum {
    ($value:expr, count) => {};
    ($value:expr, gauge) => {};
    ($value:expr, max) => {
        $value = 0;
    };
    ($value:expr, min) => {
        $value = None;
    };
}

// This inventory fixes field order and keeps interval resets beside field kinds.
macro_rules! counters {
    ($($field:ident: $kind:ident),+ $(,)?) => {
        pub(crate) struct Stats {
            $(pub(crate) $field: counter_type!($kind),)+
            last: Instant,
            closes_at_line: u64,
            episode: bool,
        }

        impl Stats {
            pub(crate) fn new(now: Instant) -> Self {
                Self {
                    $($field: Default::default(),)+
                    last: now,
                    closes_at_line: 0,
                    episode: false,
                }
            }

            // One complete line, whole or not at all (stderr_line): a dropped line leaves the
            // extrema it would have reset covering the interval since the last written line; any
            // other write failure ends the process before an extremum is reset.
            fn write_line(&mut self) {
                let time = SystemTime::now();
                let written = stderr_line(|out| {
                    write!(out, "stats t={}", unix_floor(time))?;
                    $(write!(out, concat!(" ", stringify!($field), "={}"), Number(self.$field))?;)+
                    writeln!(out)
                });
                match written {
                    Ok(true) => {
                        $(reset_extremum!(self.$field, $kind);)+
                    }
                    Ok(false) => {}
                    Err(_) => std::process::exit(1),
                }
            }
        }
    };
}

impl Stats {
    pub(crate) fn emit_if_due(&mut self, now: Instant) {
        let Some(elapsed) = now.checked_duration_since(self.last) else {
            return;
        };
        let closes = self
            .closed_eof
            .saturating_add(self.closed_reset)
            .saturating_add(self.closed_error)
            .saturating_add(self.closed_stall);
        // The four closure-start counters are cumulative and their sum saturates, so closes never
        // falls below the value stored at the last written line.
        let since = closes.strict_sub(self.closes_at_line);
        if elapsed >= EMIT_PERIOD {
            self.write_line();
            self.last = now;
            self.closes_at_line = closes;
            // A regular line at live 0 is also the end line of an open episode.
            if self.episode && self.live == 0 {
                self.episode = false;
            }
        } else if !self.episode && since >= WIND_DOWN_CLOSES {
            self.write_line();
            self.closes_at_line = closes;
            self.episode = true;
        } else if self.episode && self.live == 0 {
            self.write_line();
            self.closes_at_line = closes;
            self.episode = false;
        }
    }
}

counters! {
    turns: count, turn_us_max: max,
    wake_count: count, wake_timeout: count, wake_idle: count,
    cap_hits: count, reaped_max: max,
    ring_enters: count,
    live_r0_max: max, live_r1_max: max, live_r2_max: max, live_r3_max: max,
    live_r4_max: max, live_r5_max: max, live_r6_max: max, live_r7_max: max,
    accepted: count, accept_handoffs: count, accept_errors: count,
    enobufs: count, rearms: count, rearm_max: max,
    poll_fires: count,
    input_bytes_max: max, input_full: count,
    output_bytes_max: max, output_full: count, paused_conns_max: max,
    pipelined_waits: count, ready_served: count,
    early_replies: count,
    auth_ok: count, auth_fail: count,
    feeds: count, posts: count, healths: count,
    reader_ok: count, reader_not_found: count, reader_space: count,
    fallback_txn: count, fallback_cookie: count, fallback_cursor: count,
    fallback_record: count, fallback_span: count, fallback_missing: count,
    checked: count,
    windows: count, close_due: count, close_full: count, close_idle: count,
    w_us_min: min, w_us_max: max, writes_per_commit_max: max, write_wait_us_max: max,
    batch_commits: count,
    write_queue_max: max, write_queue_full: count,
    commit_fail: count, auto_rollback: count, replies_500: count,
    passes: count, pass_busy: count, pass_errors: count, slices: count,
    fsyncs: count, fsync_us_max: max,
    gate_holds: count, gate_hold_us_max: max,
    wal_restarts: count, wal_frames_max: max, commit_frames_max: max, header_sync_us_max: max,
    restart_writes: count,
    cache_hit: count, cache_miss: count,
    closed_eof: count, closed_reset: count, closed_error: count, closed_stall: count,
    half_close: count, close_waits: count, stale_cqe: count,
    enter_retries: count, cq_overflow: count, sq_full: count,
    stalls: count,
    chunked_bodies: count, http10_requests: count, continues: count,
    framing_errors: count, close_after_reply: count,
    ingress_held: count, ingress_held_max: max,
    live: gauge, outq_errors: count, linger_errors: count,
}

struct Number<T>(T);

impl fmt::Display for Number<u64> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl fmt::Display for Number<Option<u64>> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Some(value) => value.fmt(f),
            None => f.write_str("na"),
        }
    }
}

fn unix_floor(time: SystemTime) -> i128 {
    match time.duration_since(UNIX_EPOCH) {
        Ok(duration) => i128::from(duration.as_secs()),
        Err(error) => {
            let duration = error.duration();
            -i128::from(duration.as_secs()) - i128::from(duration.subsec_nanos() != 0)
        }
    }
}

/// Writes one line to stderr in one write, whole or not at all, so that no stderr write can stall
/// the loop or the watchdog once main has made stderr nonblocking. `render` writes the line, its
/// final LF included, into a LINE_MAX buffer; a longer line, or one stderr cannot take at once (a
/// full pipe or socket refuses the nonblocking write), is dropped and counted in DROPPED_LINES.
///
/// # Errors
/// Returns any other write failure. Ok(true) means the line went whole, Ok(false) that it was
/// dropped.
pub(crate) fn stderr_line(
    render: impl FnOnce(&mut dyn Write) -> io::Result<()>,
) -> io::Result<bool> {
    let mut line = [0_u8; LINE_MAX];
    let mut rest = line.as_mut_slice();
    // A write into a byte slice fails only past its end, when the line is longer than LINE_MAX.
    if render(&mut rest).is_ok() {
        // rest is the unwritten suffix of line, so its length is at most LINE_MAX.
        let len = LINE_MAX.strict_sub(rest.len());
        let line = line
            .get(..len)
            .expect("the rendered length is within LINE_MAX");
        let result = io::stderr().lock().write(line);
        match result {
            Ok(written) if written == line.len() => return Ok(true),
            // A pipe never takes part of a write of at most PIPE_BUF bytes; a short write elsewhere
            // cuts the line and counts as a drop.
            Ok(_) => {}
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                ) => {}
            Err(error) => return Err(error),
        }
    }
    // Relaxed: the count publishes nothing else; it wraps only after 2^64 drops.
    DROPPED_LINES.fetch_add(1, Ordering::Relaxed);
    Ok(false)
}

// Shared: the loop writes turn and step and the watchdog thread reads them and writes stalls; Arc because the watchdog outlives start_watchdog, Weak on its side so it stops once the server drops the last Arc.
pub(crate) struct Progress {
    pub(crate) turn: AtomicU64,
    pub(crate) step: AtomicU8,
    pub(crate) stalls: AtomicU64,
}

pub(crate) fn start_watchdog() -> Arc<Progress> {
    let progress = Arc::new(Progress {
        turn: AtomicU64::new(0),
        step: AtomicU8::new(0),
        stalls: AtomicU64::new(0),
    });
    let weak = Arc::downgrade(&progress);
    if let Err(error) = thread::Builder::new()
        .name("twelve-watchdog".into())
        .spawn(move || watch(weak))
    {
        let mut stderr = io::stderr().lock();
        if write!(stderr, "watchdog start failed: ").is_err() {
            std::process::exit(1);
        }
        // Thread creation is the only nonnumeric diagnostic; keep it on one line.
        for c in error.to_string().chars() {
            let c = if matches!(c, '\r' | '\n') { ' ' } else { c };
            if write!(stderr, "{c}").is_err() {
                std::process::exit(1);
            }
        }
        let _ = writeln!(stderr);
        std::process::exit(1);
    }
    progress
}

fn watch(progress: Weak<Progress>) {
    let mut previous = 0;
    let mut since = Instant::now();
    let mut reported = false;
    loop {
        thread::sleep(Duration::from_millis(500));
        let Some(current) = progress.upgrade() else {
            return;
        };
        // These atomics publish only their own numeric values, not other memory.
        let turn = current.turn.load(Ordering::Relaxed);
        let now = Instant::now();
        if turn != previous {
            previous = turn;
            since = now;
            reported = false;
        } else if !reported && now.duration_since(since) >= Duration::from_secs(2) {
            // Relaxed: stalls and step publish only their own values; the watchdog reads no other data through them.
            // Relaxed on success and failure: the saturating increment publishes nothing else, and a failed attempt only rereads the counter it retries; fetch_update stays because the count saturates at u64::MAX and update/try_update are not stable on Rust 1.94.0.
            let _ = current
                .stalls
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
                    Some(n.saturating_add(1))
                });
            let step = current.step.load(Ordering::Relaxed);
            // The stall line goes out whole or not at all (stderr_line); stalls still counts a
            // dropped one.
            if stderr_line(|out| writeln!(out, "loop stalled: turn={turn} step={step}")).is_err() {
                std::process::exit(1);
            }
            reported = true;
        }
    }
}
