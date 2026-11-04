use std::collections::{HashMap, HashSet};
use std::os::fd::RawFd;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{Receiver, Sender};
use std::sync::Arc;
use std::thread;

use crate::db::ReadDb;
use crate::response_bytes;
use crate::uring::ReadJob;

#[derive(Clone)]
pub struct PostResponse {
    pub normal: Arc<Vec<u8>>,
    pub close: Option<Arc<Vec<u8>>>,
}

pub struct FeedRequest {
    pub jobs: Vec<ReadJob>,
    pub reply: Sender<FeedReply>,
    pub event_fd: RawFd,
}

pub struct FeedReply {
    pub body: Option<Arc<Vec<u8>>>,
}

pub struct PostRequest {
    pub jobs: Vec<ReadJob>,
    pub reply: Sender<PostReply>,
    pub event_fd: RawFd,
}

pub struct PostReply {
    pub packets: Option<Arc<HashMap<i64, Option<Arc<PostResponse>>>>>,
}

fn refresh_if_dirty(read: &mut ReadDb, dirty: &AtomicU64, seen: &mut u64) {
    let generation = dirty.load(Ordering::Relaxed);
    if generation != *seen {
        read.invalidate_feed();
        read.invalidate_post();
        read.invalidate_like();
        *seen = generation;
    }
    read.refresh();
}

pub fn spawn_feed(path: String, rx: Receiver<FeedRequest>, dirty: Arc<AtomicU64>) {
    thread::Builder::new()
        .name("feed-reader".into())
        .spawn(move || {
            let mut read = match ReadDb::open(&path) {
                Ok(read) => read,
                Err(e) => {
                    eprintln!("feed reader db open failed: {e}");
                    return;
                }
            };
            let mut seen = dirty.load(Ordering::Relaxed);
            while let Ok(first) = rx.recv() {
                let mut requests = vec![first];
                while let Ok(req) = rx.try_recv() {
                    requests.push(req);
                }
                refresh_if_dirty(&mut read, &dirty, &mut seen);
                let body = read.feed().ok().map(Arc::new);
                for req in requests {
                    if req.reply.send(FeedReply { body: body.clone() }).is_err() {
                        return;
                    }
                    let one = 1u64;
                    unsafe {
                        libc::write(
                            req.event_fd,
                            &one as *const u64 as *const libc::c_void,
                            std::mem::size_of::<u64>(),
                        );
                    }
                }
            }
        });
}

pub fn spawn_post(path: String, rx: Receiver<PostRequest>, dirty: Arc<AtomicU64>) {
    thread::Builder::new()
        .name("post-reader".into())
        .spawn(move || {
            let mut read = match ReadDb::open(&path) {
                Ok(read) => read,
                Err(e) => {
                    eprintln!("post reader db open failed: {e}");
                    return;
                }
            };
            let mut seen = dirty.load(Ordering::Relaxed);
            while let Ok(first) = rx.recv() {
                let mut requests = vec![first];
                while let Ok(req) = rx.try_recv() {
                    requests.push(req);
                }
                refresh_if_dirty(&mut read, &dirty, &mut seen);
                let mut ids = HashSet::new();
                for req in &requests {
                    for job in &req.jobs {
                        if let crate::uring::ReadKind::Post(id) = job.kind {
                            ids.insert(id);
                        }
                    }
                }
                let bodies = read.posts(&ids.into_iter().collect::<Vec<_>>()).ok();
                let packets: Option<Arc<HashMap<i64, Option<Arc<PostResponse>>>>> = bodies.map(|bodies| {
                    Arc::new(
                        bodies
                            .into_iter()
                            .map(|(id, body)| {
                                let response = body.map(|body| {
                                    let normal = Arc::new(response_bytes(200, &body, false));
                                    let close = Arc::new(response_bytes(200, &body, true));
                                    Arc::new(PostResponse {
                                        normal,
                                        close: Some(close),
                                    })
                                });
                                (id, response)
                            })
                            .collect(),
                    )
                });
                for req in requests {
                    if req.reply.send(PostReply { packets: packets.clone() }).is_err() {
                        return;
                    }
                    let one = 1u64;
                    unsafe {
                        libc::write(
                            req.event_fd,
                            &one as *const u64 as *const libc::c_void,
                            std::mem::size_of::<u64>(),
                        );
                    }
                }
            }
        });
}
