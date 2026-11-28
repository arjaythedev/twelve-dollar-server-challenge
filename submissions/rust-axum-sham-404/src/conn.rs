//! Connection accounting and keep-alive retirement.
//!
//! The inherited `nofile` limit is per process. When the server approaches its
//! connection budget, completed responses advertise `Connection: close`, so the
//! socket is released and clients waiting in the listen queue can be accepted.
//! This keeps the descriptor table below the hard limit instead of parking users
//! until their connect timeout.

use std::sync::{
    Arc, OnceLock,
    atomic::{AtomicUsize, Ordering},
};

use axum::{extract::connect_info::Connected, serve::IncomingStream};

/// Retire keep-alive connections well before the hard descriptor limit.
const KEEPALIVE_BUDGET: usize = 40_000;
/// Descriptors kept free for SQLite, the listener and transient sockets.
const RESERVED_DESCRIPTORS: usize = 128;

static OPEN: AtomicUsize = AtomicUsize::new(0);
static BUDGET: OnceLock<usize> = OnceLock::new();

/// True when completed responses should ask the client to close its connection.
pub fn should_retire() -> bool {
    OPEN.load(Ordering::Relaxed) >= *BUDGET.get_or_init(|| budget_for(soft_nofile()))
}

fn soft_nofile() -> Option<u64> {
    let mut limit = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    // SAFETY: getrlimit only writes into the provided `rlimit` structure.
    if unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut limit) } == 0 {
        Some(limit.rlim_cur)
    } else {
        None
    }
}

/// 40,000 connections, or 128 fewer than a smaller inherited limit.
fn budget_for(soft: Option<u64>) -> usize {
    match soft {
        Some(soft) => {
            let soft = soft.min(usize::MAX as u64) as usize;
            KEEPALIVE_BUDGET.min(soft.saturating_sub(RESERVED_DESCRIPTORS).max(1))
        }
        None => KEEPALIVE_BUDGET,
    }
}

/// Lives as long as its connection; the final clone drops with the connection.
struct Lease;

impl Drop for Lease {
    fn drop(&mut self) {
        OPEN.fetch_sub(1, Ordering::Relaxed);
    }
}

/// Per-connection metadata that keeps the open-connection count accurate.
#[derive(Clone)]
pub struct ConnectionInfo {
    _lease: Arc<Lease>,
}

impl Connected<IncomingStream<'_>> for ConnectionInfo {
    fn connect_info(_: IncomingStream<'_>) -> Self {
        OPEN.fetch_add(1, Ordering::Relaxed);
        Self {
            _lease: Arc::new(Lease),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn budget_leaves_room_below_the_hard_limit() {
        assert_eq!(budget_for(Some(65_535)), 40_000);
        assert_eq!(budget_for(Some(40_128)), 40_000);
        assert_eq!(budget_for(Some(1_000)), 872);
        assert_eq!(budget_for(Some(128)), 1);
        assert_eq!(budget_for(Some(0)), 1);
        assert_eq!(budget_for(None), 40_000);
    }
}
