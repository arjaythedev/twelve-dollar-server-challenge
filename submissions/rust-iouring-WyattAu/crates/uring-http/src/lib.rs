//! io_uring HTTP/1.1 kit: one thread, multishot accept/recv, provided buffer
//! rings, fixed-shape JSON response templates, batched loop passes.
//!
//! Design source: the top of the arjaythedev $12-server leaderboard
//! (io_uring + provided buffers + pass-per-transaction + flush-after-COMMIT).
//! Kernel note: Ubuntu 6.8.0-136+ refuses provided-buffer-ring registration
//! with zeroed reserved words (Launchpad #2162843) — retry once with
//! `resv[0] = 1`.

#![forbid(unsafe_op_in_unsafe_fn)]

pub mod http;
pub mod json;
pub mod loop_;
