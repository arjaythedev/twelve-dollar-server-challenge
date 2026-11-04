use std::collections::HashSet;
use std::os::fd::RawFd;
use std::sync::mpsc::{Receiver, Sender};
use std::thread;
use std::time::Instant;

use crate::db::WriteDb;
use crate::{error, json_string, WriteCmd, WriteKind};

pub struct WriteJob {
    pub ring: usize,
    pub slot: u32,
    pub gen: u32,
    pub close: bool,
    pub at: Option<Instant>,
    pub stat: usize,
    pub cmd: WriteCmd,
}

pub struct WriteResult {
    pub ring: usize,
    pub slot: u32,
    pub gen: u32,
    pub status: u16,
    pub body: Vec<u8>,
    pub close: bool,
    pub feed_dirty: bool,
    pub like_dirty: bool,
    pub at: Option<Instant>,
    pub stat: usize,
}

pub fn spawn(
    path: String,
    rx: Receiver<WriteJob>,
    tx: Sender<WriteResult>,
    event_fd: RawFd,
    ready: Sender<()>,
) {
    thread::Builder::new()
        .name("writer".into())
        .spawn(move || {
        let mut db = match WriteDb::open(&path) {
            Ok(db) => db,
            Err(e) => {
                eprintln!("writer db open failed: {e}");
                return;
            }
        };
        let _ = ready.send(());
        loop {
            let first = match rx.recv() {
                Ok(job) => job,
                Err(_) => break,
            };

            let mut jobs = Vec::with_capacity(64);
            jobs.push(first);
            while let Ok(job) = rx.try_recv() {
                jobs.push(job);
                if jobs.len() >= 64 {
                    break;
                }
            }

            let began = db.begin().is_ok();
            let mut committed = false;
            let mut results: Vec<(u16, Vec<u8>)> = (0..jobs.len())
                .map(|_| (500, error("internal server error")))
                .collect();

            let mut pair_count = 0usize;
            if began {
                {
                    let mut create = db
                        .prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
                        .ok();
                    let mut exists = db.prepare("SELECT 1 FROM posts WHERE id = ?1").ok();
                    let mut bulk_like = db
                        .prepare(
                            "INSERT INTO likes (user_id, post_id)
                             SELECT CAST(value->>0 AS INTEGER), CAST(value->>1 AS INTEGER)
                               FROM json_each(?1)
                              WHERE EXISTS (SELECT 1 FROM posts
                                             WHERE id = CAST(value->>1 AS INTEGER))
                              ON CONFLICT (user_id, post_id) DO NOTHING
                             RETURNING user_id, post_id",
                        )
                        .ok();

                    let mut pairs: Vec<[i64; 2]> = Vec::new();
                    let mut pair_jobs: Vec<usize> = Vec::new();

                    for (i, job) in jobs.iter().enumerate() {
                        match (&mut create, &job.cmd.kind) {
                            (Some(create), WriteKind::Create {
                                user_id,
                                username,
                                body,
                            }) => {
                                create.reset();
                                create.bind_i64(1, *user_id);
                                create.bind_text(2, body);
                                if let Ok(true) = create.step() {
                                    let id = create.column_i64(0);
                                    let created_at = create.column_text(1);
                                    let body_json = json_string(body);
                                    let author_json = json_string(username);
                                    let created_json = json_string(&created_at);
                                    results[i] = (
                                        201,
                                        format!(
                                            "{{\"post\":{{\"id\":{id},\"body\":{body_json},\"created_at\":{created_json},\"author\":{author_json},\"like_count\":0}}}}"
                                        )
                                        .into_bytes(),
                                    );
                                }
                            }
                            (_, WriteKind::Like { user_id, post_id }) => {
                                pairs.push([*user_id, *post_id]);
                                pair_jobs.push(i);
                            }
                            _ => {}
                        }
                    }

                    pair_count = pairs.len();
                    if !pairs.is_empty() {
                        if let Some(bulk_like) = bulk_like.as_mut() {
                            let json = serde_json::to_string(&pairs).unwrap_or_else(|_| "[]".to_string());
                            bulk_like.reset();
                            bulk_like.bind_text(1, &json);
                            let mut inserted: HashSet<(i64, i64)> = HashSet::with_capacity(pairs.len());
                            let mut bulk_ok = true;
                            loop {
                                match bulk_like.step() {
                                    Ok(true) => {
                                        inserted.insert((bulk_like.column_i64(0), bulk_like.column_i64(1)));
                                    }
                                    Ok(false) => break,
                                    Err(_) => {
                                        bulk_ok = false;
                                        break;
                                    }
                                }
                            }
                            if bulk_ok {
                                for (pair, i) in pairs.iter().zip(pair_jobs.iter()) {
                                    if inserted.contains(&(pair[0], pair[1])) {
                                        results[*i] = (
                                            201,
                                            format!(
                                                "{{\"liked\":true,\"already_liked\":false,\"post_id\":{}}}",
                                                pair[1]
                                            )
                                            .into_bytes(),
                                        );
                                    } else if let Some(exists) = exists.as_mut() {
                                        exists.reset();
                                        exists.bind_i64(1, pair[1]);
                                        match exists.step() {
                                            Ok(true) => {
                                                results[*i] = (
                                                    200,
                                                    format!(
                                                        "{{\"liked\":true,\"already_liked\":true,\"post_id\":{}}}",
                                                        pair[1]
                                                    )
                                                    .into_bytes(),
                                                );
                                            }
                                            Ok(false) => {
                                                results[*i] = (404, error("post not found"));
                                            }
                                            Err(_) => {}
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                committed = db.commit().is_ok();
                if !committed {
                    let _ = db.rollback();
                }
            }

            for (i, job) in jobs.into_iter().enumerate() {
                let (status, body) = if committed {
                    results.get(i).cloned().unwrap_or_else(|| (500, error("internal server error")))
                } else {
                    (500, error("internal server error"))
                };
                let close = job.close || !committed;
                let feed_dirty = matches!(job.cmd.kind, WriteKind::Create { .. });
                let like_dirty = matches!(job.cmd.kind, WriteKind::Like { .. });
                if tx
                    .send(WriteResult {
                        ring: job.ring,
                        slot: job.slot,
                        gen: job.gen,
                        status,
                        body,
                        close,
                        feed_dirty,
                        like_dirty,
                        at: job.at,
                        stat: job.stat,
                    })
                    .is_err()
                {
                    return;
                }
            }
            let one = 1u64;
            unsafe {
                libc::write(
                    event_fd,
                    &one as *const u64 as *const libc::c_void,
                    std::mem::size_of::<u64>(),
                );
            }
        }
    });
}
