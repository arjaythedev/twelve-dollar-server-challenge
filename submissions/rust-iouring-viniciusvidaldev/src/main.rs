use std::{ffi::CString, io, net::TcpListener, os::fd::AsRawFd};

use crate::{
    app::{App, Auth},
    db::Db,
    eventfd::new_eventfd,
    server::Server,
    sys::{cvt, socket},
    uring::IoUring,
};

mod app;
mod db;
mod eventfd;
mod http;
mod mmap;
mod rlimit;
mod server;
mod sys;
mod uring;

const SQE_ENTRIES: u32 = 4096;
// >= file slots per ring: each conn has at most one recv/send in flight, so the cq can't overflow
const CQE_ENTRIES: u32 = 65536;

// provided buffers
const BUF_ENTRIES: u16 = 16384;
const BUF_SIZE: u32 = 1024;

const RINGS: usize = 5;
const FILE_SLOTS: u64 = 65535;

fn main() -> io::Result<()> {
    let path = std::env::var("SQLITE_PATH").expect("SQLITE_PATH not set");
    let path = CString::new(path).expect("SQLITE_PATH contains a nul byte");

    let db = Db::open(&path)?;

    let secret = std::env::var("JWT_SECRET").expect("JWT_SECRET not set");
    let auth = Auth::new(secret.as_bytes()).map_err(io::Error::other)?;

    let app = App::new(db, auth);

    let host = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into());
    let port = std::env::var("PORT").unwrap_or_else(|_| "8080".into());
    let listener = TcpListener::bind(format!("{host}:{port}"))?;
    cvt(unsafe { socket::listen(listener.as_raw_fd(), 4096) })?;

    // Raises soft fd limit.
    let nofile = rlimit::raise_nofile()?;

    let slots = nofile.min(FILE_SLOTS) as u32;

    let mut rings = Vec::with_capacity(RINGS);
    let mut wakers = Vec::with_capacity(RINGS - 1);
    for i in 0..RINGS {
        let mut ring = IoUring::new(SQE_ENTRIES, CQE_ENTRIES)?;
        ring.setup_buf_ring(BUF_ENTRIES, BUF_SIZE)?;
        ring.register_files_sparse(slots)?;
        if i > 0 {
            let efd = new_eventfd()?;
            ring.register_eventfd(efd.as_raw_fd())?;
            wakers.push(efd);
        }
        rings.push(ring);
    }

    let mut server = Server::new(rings, wakers, listener, app);
    server.run()
}
