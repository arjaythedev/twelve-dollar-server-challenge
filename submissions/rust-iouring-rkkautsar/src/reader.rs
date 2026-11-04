use std::collections::{HashMap, HashSet};
use std::os::fd::RawFd;
use std::sync::mpsc::{Receiver, Sender};
use std::sync::Arc;
use std::thread;

use crate::db::ReadDb;
use crate::response_bytes;
use crate::stats;
use crate::uring::{ReadJob, ReadKind};

pub struct ReadBatch {
    pub items: Vec<ReadJob>,
}

pub enum ReadMsg {
    Dirty { feed: bool, like: bool },
    Batch(Arc<ReadBatch>),
}

#[derive(Clone)]
pub struct PostResponse {
    pub normal: Arc<Vec<u8>>,
    pub close: Option<Arc<Vec<u8>>>,
}

pub struct ReadResponse {
    pub feed: Option<Vec<u8>>,
    pub posts: Vec<Option<PostResponse>>,
}

pub fn spawn(
    path: String,
    rx: Receiver<ReadMsg>,
    tx: Sender<ReadResponse>,
    event_fd: RawFd,
) {
    thread::Builder::new()
        .name("reader".into())
        .spawn(move || {
        let mut read = match ReadDb::open(&path) {
            Ok(read) => read,
            Err(e) => {
                eprintln!("reader db open failed: {e}");
                return;
            }
        };
        while let Ok(msg) = rx.recv() {
            match msg {
                ReadMsg::Dirty { feed, like } => {
                    if feed {
                        read.invalidate_feed();
                        read.invalidate_post();
                    }
                    if like {
                        read.invalidate_like();
                    }
                }
                ReadMsg::Batch(batch) => {
                    let t = stats::mark();
                    read.refresh();
                    stats::phase(stats::R_REFRESH, t);
                    let need_feed = batch.items.iter().any(|j| matches!(j.kind, ReadKind::Feed));
                    let t = stats::mark();
                    let feed = if need_feed { read.feed().ok() } else { None };
                    stats::phase(stats::R_FEED, t);
                    let ids: Vec<i64> = batch
                        .items
                        .iter()
                        .filter_map(|j| match j.kind {
                            ReadKind::Post(id) => Some(id),
                            _ => None,
                        })
                        .collect();
                    let t = stats::mark();
                    let map = if ids.is_empty() {
                        None
                    } else {
                        read.posts(&ids).ok()
                    };
                    stats::phase(stats::R_POSTS, t);
                    let t = stats::mark();
                    let mut close_ids: HashSet<i64> = HashSet::new();
                    for j in &batch.items {
                        if j.close {
                            if let ReadKind::Post(id) = j.kind {
                                close_ids.insert(id);
                            }
                        }
                    }
                    let packets: HashMap<i64, Option<PostResponse>> = map
                        .unwrap_or_default()
                        .into_iter()
                        .map(|(id, body)| {
                            let normal = body.as_ref().map(|b| Arc::new(response_bytes(200, b, false)));
                            let close = if close_ids.contains(&id) {
                                body.as_ref().map(|b| Arc::new(response_bytes(200, b, true)))
                            } else {
                                None
                            };
                            let resp = normal.map(|normal| PostResponse { normal, close });
                            (id, resp)
                        })
                        .collect();
                    let posts = batch
                        .items
                        .iter()
                        .filter_map(|j| match j.kind {
                            ReadKind::Post(id) => packets.get(&id).cloned(),
                            _ => None,
                        })
                        .collect();
                    stats::phase(stats::R_PACKET, t);
                    let t = stats::mark();
                    if tx.send(ReadResponse { feed, posts }).is_err() {
                        return;
                    }
                    stats::phase(stats::R_SEND, t);
                    let one = 1u64;
                    unsafe {
                        libc::write(
                            event_fd,
                            &one as *const u64 as *const libc::c_void,
                            std::mem::size_of::<u64>(),
                        );
                    }
                }
            }
        }
    });
}
