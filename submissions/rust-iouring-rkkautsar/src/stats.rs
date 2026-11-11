use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::OnceLock;
use std::time::Instant;

pub const FEED: usize = 0;
pub const POST: usize = 1;
pub const LIKE: usize = 2;
pub const CREATE: usize = 3;
pub const R_REFRESH: usize = 4;
pub const R_FEED: usize = 5;
pub const R_POSTS: usize = 6;
pub const R_PACKET: usize = 7;
pub const R_SEND: usize = 8;

const NAMES: [&str; 9] = [
    "feed", "post", "like", "create",
    "r_refresh", "r_feed", "r_posts", "r_packet", "r_send",
];
const BUCKETS: usize = 1024;
const PBUCKETS: usize = 24;

static CNT: [AtomicU64; 9] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0),
];
static SUM: [AtomicU64; 9] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0),
];
static MAX: [AtomicU64; 9] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0),
];
static HIST: OnceLock<Vec<AtomicU64>> = OnceLock::new();

fn hist() -> &'static [AtomicU64] {
    HIST.get_or_init(|| (0..9 * BUCKETS).map(|_| AtomicU64::new(0)).collect())
}

static PCNT: [AtomicU64; 5] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0), AtomicU64::new(0),
];
static PSUM: [AtomicU64; 5] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0), AtomicU64::new(0),
];
static PMAX: [AtomicU64; 5] = [
    AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0),
    AtomicU64::new(0), AtomicU64::new(0),
];
static PHIST: OnceLock<Vec<AtomicU64>> = OnceLock::new();

fn phist() -> &'static [AtomicU64] {
    PHIST.get_or_init(|| (0..5 * PBUCKETS).map(|_| AtomicU64::new(0)).collect())
}

pub fn enabled() -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("STATS").map(|v| v == "1").unwrap_or(false))
}

pub fn mark() -> Option<Instant> {
    if enabled() { Some(Instant::now()) } else { None }
}

fn pct(h: &[AtomicU64], total: u64, q: u64) -> u64 {
    if total == 0 {
        return 0;
    }
    let target = (total * q + 99) / 100;
    let mut seen = 0u64;
    for (i, b) in h.iter().enumerate() {
        seen += b.load(Ordering::Relaxed);
        if seen >= target {
            return (i + 1) as u64;
        }
    }
    BUCKETS as u64
}

pub fn record(kind: usize, at: Option<Instant>) {
    let Some(at) = at else { return };
    let ns = at.elapsed().as_nanos() as u64;
    let n = CNT[kind].fetch_add(1, Ordering::Relaxed) + 1;
    SUM[kind].fetch_add(ns, Ordering::Relaxed);
    let mut old = MAX[kind].load(Ordering::Relaxed);
    while ns > old {
        match MAX[kind].compare_exchange_weak(old, ns, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(v) => old = v,
        }
    }
    let ms = (ns / 1_000_000) as usize;
    hist()[kind * BUCKETS + ms.min(BUCKETS - 1)].fetch_add(1, Ordering::Relaxed);
    if n % 8192 == 0 {
        let sum = SUM[kind].load(Ordering::Relaxed);
        eprintln!(
            "stats {} n={} avg_ms={} p50_ms={} p95_ms={} p99_ms={} p999_ms={} max_ms={}",
            NAMES[kind],
            n,
            sum / n / 1_000_000,
            pct(&hist()[kind * BUCKETS..(kind + 1) * BUCKETS], n, 50),
            pct(&hist()[kind * BUCKETS..(kind + 1) * BUCKETS], n, 95),
            pct(&hist()[kind * BUCKETS..(kind + 1) * BUCKETS], n, 99),
            pct(&hist()[kind * BUCKETS..(kind + 1) * BUCKETS], n, 999),
            MAX[kind].load(Ordering::Relaxed) / 1_000_000
        );
    }
}

pub fn phase(kind: usize, at: Option<Instant>) {
    let Some(at) = at else { return };
    let k = kind - R_REFRESH;
    let ns = at.elapsed().as_nanos() as u64;
    let us = (ns / 1000).max(1) as u64;
    let n = PCNT[k].fetch_add(1, Ordering::Relaxed) + 1;
    PSUM[k].fetch_add(ns, Ordering::Relaxed);
    let mut old = PMAX[k].load(Ordering::Relaxed);
    while ns > old {
        match PMAX[k].compare_exchange_weak(old, ns, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(v) => old = v,
        }
    }
    let b = (us.ilog2() as usize).min(PBUCKETS - 1);
    phist()[k * PBUCKETS + b].fetch_add(1, Ordering::Relaxed);
    if n % 16384 == 0 {
        let sum = PSUM[k].load(Ordering::Relaxed);
        let target = (n * 99 + 99) / 100;
        let mut seen = 0u64;
        let mut p99 = 0u64;
        for (i, bucket) in phist()[k * PBUCKETS..(k + 1) * PBUCKETS].iter().enumerate() {
            seen += bucket.load(Ordering::Relaxed);
            if seen >= target {
                p99 = 1u64 << i;
                break;
            }
        }
        eprintln!(
            "phase {} n={} avg_us={} p99_us={} max_us={}",
            NAMES[kind],
            n,
            sum / n / 1000,
            p99,
            PMAX[k].load(Ordering::Relaxed) / 1000
        );
    }
}

/// Clears every counter so a later printout covers only the scored hold.
pub fn reset() {
    for a in CNT.iter().chain(SUM.iter()).chain(MAX.iter()) {
        a.store(0, Ordering::Relaxed);
    }
    for a in hist().iter() {
        a.store(0, Ordering::Relaxed);
    }
    for a in PCNT.iter().chain(PSUM.iter()).chain(PMAX.iter()) {
        a.store(0, Ordering::Relaxed);
    }
    for a in phist().iter() {
        a.store(0, Ordering::Relaxed);
    }
}
