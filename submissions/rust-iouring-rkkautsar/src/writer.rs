use std::collections::HashSet;
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender};
use std::thread;
use std::time::{Duration, Instant};

use crate::db::{Stmt, WriteDb};
use crate::{error, json_string, WriteCmd, WriteKind};

const POST_INSERT_SQL: &str = "INSERT INTO posts (user_id, body)
                     SELECT CAST(value->>0 AS INTEGER), CAST(value->>1 AS TEXT)
                       FROM json_each(?1)
                     RETURNING id, created_at";
const LIKE_INSERT_SQL: &str = "INSERT INTO likes (user_id, post_id)
                     SELECT CAST(value->>0 AS INTEGER), CAST(value->>1 AS INTEGER)
                       FROM json_each(?1)
                      WHERE 1
                      ON CONFLICT (user_id, post_id) DO NOTHING
                     RETURNING user_id, post_id";
const LIKE_INSERT_FILTERED_SQL: &str = "INSERT INTO likes (user_id, post_id)
                     SELECT CAST(value->>0 AS INTEGER), CAST(value->>1 AS INTEGER)
                       FROM json_each(?1)
                      WHERE EXISTS (SELECT 1 FROM posts
                                     WHERE id = CAST(value->>1 AS INTEGER))
                      ON CONFLICT (user_id, post_id) DO NOTHING
                     RETURNING user_id, post_id";
const POST_EXISTS_SQL: &str = "SELECT CAST(value AS INTEGER)
                       FROM json_each(?1)
                      WHERE CAST(value AS INTEGER) IN (SELECT id FROM posts)";

pub struct BatchCache {
    post_insert: Option<Stmt<'static>>,
    like_insert: Option<Stmt<'static>>,
    like_insert_filtered: Option<Stmt<'static>>,
    post_exists: Option<Stmt<'static>>,
}

impl BatchCache {
    pub fn new(db: &WriteDb) -> Self {
        Self {
            post_insert: db.prepare_static(POST_INSERT_SQL).ok(),
            like_insert: db.prepare_static(LIKE_INSERT_SQL).ok(),
            like_insert_filtered: db.prepare_static(LIKE_INSERT_FILTERED_SQL).ok(),
            post_exists: db.prepare_static(POST_EXISTS_SQL).ok(),
        }
    }
}

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
    result_tx: Sender<WriteResult>,
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
        let mut cache = BatchCache::new(&db);
        let _ = ready.send(());
        let window_us = std::env::var("WRITE_COALESCE_US")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .unwrap_or(0);
        // Capacity retained across batches: 2,048 x sizeof(WriteJob) is a
        // 245 KB allocation the writer would otherwise repeat per batch.
        let mut jobs: Vec<WriteJob> = Vec::with_capacity(2048);
        loop {
            let first = match rx.recv() {
                Ok(job) => job,
                Err(_) => break,
            };

            jobs.clear();
            jobs.push(first);
            if window_us == 0 {
                while let Ok(job) = rx.try_recv() {
                    jobs.push(job);
                    if jobs.len() >= 2048 {
                        break;
                    }
                }
            } else {
                let deadline = Instant::now() + Duration::from_micros(window_us);
                while jobs.len() < 2048 {
                    let now = Instant::now();
                    let Some(remaining) = deadline.checked_duration_since(now) else {
                        break;
                    };
                    match rx.recv_timeout(remaining) {
                        Ok(job) => jobs.push(job),
                        Err(RecvTimeoutError::Timeout) | Err(RecvTimeoutError::Disconnected) => break,
                    }
                }
            }

            let (results, committed) = run_batch(&mut db, &mut cache, &jobs);
            let mut results = results.into_iter();
            for job in jobs.drain(..) {
                let (status, body) = if committed {
                    results.next().unwrap_or_else(|| (500, error("internal server error")))
                } else {
                    (500, error("internal server error"))
                };
                let close = job.close || !committed;
                let feed_dirty = matches!(job.cmd.kind, WriteKind::Create { .. });
                let like_dirty = matches!(job.cmd.kind, WriteKind::Like { .. });
                if result_tx
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
        }
    });
}

pub fn run_batch(
    db: &mut WriteDb,
    cache: &mut BatchCache,
    jobs: &[WriteJob],
) -> (Vec<(u16, Vec<u8>)>, bool) {
    let began = db.begin().is_ok();
    let mut committed = false;
    let mut results: Vec<(u16, Vec<u8>)> = (0..jobs.len())
        .map(|_| (0, Vec::new()))
        .collect();

    let mut batch_ok = began;
    if began {
        {
            let has_create = jobs
                .iter()
                .any(|j| matches!(&j.cmd.kind, WriteKind::Create { .. }));
            let has_like = jobs
                .iter()
                .any(|j| matches!(&j.cmd.kind, WriteKind::Like { .. }));
            let mut bulk_create = if has_create {
                cache.post_insert.as_mut()
            } else {
                None
            };
            let mut bulk_like = if has_like {
                cache.like_insert.as_mut()
            } else {
                None
            };
            let mut bulk_like_filtered = if has_like {
                cache.like_insert_filtered.as_mut()
            } else {
                None
            };
            let mut bulk_exists = if has_like {
                cache.post_exists.as_mut()
            } else {
                None
            };

            // Job indices instead of cloned bodies/usernames: the response is
            // built from the job itself, and the JSON from borrowed strings.
            let mut create_idx: Vec<usize> = Vec::new();
            let mut creates: Vec<(i64, &str)> = Vec::new();
            let mut pairs: Vec<[i64; 2]> = Vec::new();
            let mut pair_jobs: Vec<usize> = Vec::new();

            for (i, job) in jobs.iter().enumerate() {
                match &job.cmd.kind {
                    WriteKind::Create { user_id, body, .. } => {
                        create_idx.push(i);
                        creates.push((*user_id, body.as_str()));
                    }
                    WriteKind::Like { user_id, post_id } => {
                        pairs.push([*user_id, *post_id]);
                        pair_jobs.push(i);
                    }
                }
            }

            if !creates.is_empty() {
                let json = serde_json::to_string(&creates).unwrap_or_else(|_| "[]".to_string());
                let mut rows: Vec<(i64, String)> = Vec::with_capacity(creates.len());
                match bulk_create.as_mut() {
                    Some(st) => {
                        st.reset();
                        st.bind_text(1, &json);
                        loop {
                            match st.step() {
                                Ok(true) => {
                                    rows.push((st.column_i64(0), st.column_text(1)));
                                }
                                Ok(false) => break,
                                Err(_) => {
                                    batch_ok = false;
                                    break;
                                }
                            }
                        }
                    }
                    None => batch_ok = false,
                }
                if batch_ok && rows.len() == create_idx.len() {
                    rows.sort_unstable_by_key(|row| row.0);
                    for (&i, (id, created_at)) in create_idx.iter().zip(rows.iter()) {
                        let WriteKind::Create { body, username, .. } = &jobs[i].cmd.kind
                        else {
                            continue;
                        };
                        let body_json = json_string(body);
                        let author_json = json_string(username);
                        let created_json = json_string(created_at);
                        results[i] = (
                            201,
                            format!(
                                "{{\"post\":{{\"id\":{id},\"body\":{body_json},\"created_at\":{created_json},\"author\":{author_json},\"like_count\":0}}}}"
                            )
                            .into_bytes(),
                        );
                    }
                } else {
                    batch_ok = false;
                }
            }

            if batch_ok && !pairs.is_empty() {
                let json = serde_json::to_string(&pairs).unwrap_or_else(|_| "[]".to_string());
                let mut inserted: HashSet<(i64, i64)> = HashSet::with_capacity(pairs.len());
                let mut fallback = false;

                // Optimistic path: no post-existence pre-check.  The
                // FK on likes rejects a missing post; duplicate likes
                // simply return no row.
                match bulk_like.as_mut() {
                    Some(st) => {
                        st.reset();
                        st.bind_text(1, &json);
                        loop {
                            match st.step() {
                                Ok(true) => {
                                    inserted.insert((st.column_i64(0), st.column_i64(1)));
                                }
                                Ok(false) => break,
                                Err(_) => {
                                    fallback = true;
                                    break;
                                }
                            }
                        }
                    }
                    None => batch_ok = false,
                }

                let mut existing: HashSet<i64> = HashSet::new();
                if batch_ok {
                    if fallback {
                        // A missing post aborted the optimistic
                        // statement.  Resolve the whole batch, then
                        // insert only candidates that exist.
                        let unique_posts: Vec<i64> =
                            pairs.iter().map(|pair| pair[1]).collect::<HashSet<_>>().into_iter().collect();
                        let json = serde_json::to_string(&unique_posts)
                            .unwrap_or_else(|_| "[]".to_string());
                        match bulk_exists.as_mut() {
                            Some(st) => {
                                st.reset();
                                st.bind_text(1, &json);
                                loop {
                                    match st.step() {
                                        Ok(true) => {
                                            existing.insert(st.column_i64(0));
                                        }
                                        Ok(false) => break,
                                        Err(_) => {
                                            batch_ok = false;
                                            break;
                                        }
                                    }
                                }
                            }
                            None => batch_ok = false,
                        }
                        if batch_ok {
                            let candidates: Vec<[i64; 2]> = pairs
                                .iter()
                                .filter(|pair| existing.contains(&pair[1]))
                                .copied()
                                .collect();
                            if !candidates.is_empty() {
                                let json = serde_json::to_string(&candidates)
                                    .unwrap_or_else(|_| "[]".to_string());
                                match bulk_like_filtered.as_mut() {
                                    Some(st) => {
                                        st.reset();
                                        st.bind_text(1, &json);
                                        loop {
                                            match st.step() {
                                                Ok(true) => {
                                                    inserted.insert((
                                                        st.column_i64(0),
                                                        st.column_i64(1),
                                                    ));
                                                }
                                                Ok(false) => break,
                                                Err(_) => {
                                                    batch_ok = false;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    None => batch_ok = false,
                                }
                            }
                        }
                    } else {
                        // Existence is needed only for pairs that
                        // produced no row: duplicate likes.
                        let missing: Vec<i64> = pairs
                            .iter()
                            .filter(|pair| !inserted.contains(&(pair[0], pair[1])))
                            .map(|pair| pair[1])
                            .collect::<HashSet<_>>()
                            .into_iter()
                            .collect();
                        if !missing.is_empty() {
                            let json = serde_json::to_string(&missing)
                                .unwrap_or_else(|_| "[]".to_string());
                            match bulk_exists.as_mut() {
                                Some(st) => {
                                    st.reset();
                                    st.bind_text(1, &json);
                                    loop {
                                        match st.step() {
                                            Ok(true) => {
                                                existing.insert(st.column_i64(0));
                                            }
                                            Ok(false) => break,
                                            Err(_) => {
                                                batch_ok = false;
                                                break;
                                            }
                                        }
                                    }
                                }
                                None => batch_ok = false,
                            }
                        }
                    }
                }
                // An inserted pair also proves the post exists for later
                // duplicate pairs in this batch.
                existing.extend(inserted.iter().map(|&(_, post_id)| post_id));

                if batch_ok {
                    for (pair, i) in pairs.iter().zip(pair_jobs.iter()) {
                        if !fallback && inserted.remove(&(pair[0], pair[1])) {
                            results[*i] = (
                                201,
                                format!(
                                    "{{\"liked\":true,\"already_liked\":false,\"post_id\":{}}}",
                                    pair[1]
                                )
                                .into_bytes(),
                            );
                        } else if fallback && !existing.contains(&pair[1]) {
                            results[*i] = (404, error("post not found"));
                        } else if !fallback && !existing.contains(&pair[1]) {
                            results[*i] = (404, error("post not found"));
                        } else if inserted.remove(&(pair[0], pair[1])) {
                            results[*i] = (
                                201,
                                format!(
                                    "{{\"liked\":true,\"already_liked\":false,\"post_id\":{}}}",
                                    pair[1]
                                )
                                .into_bytes(),
                            );
                        } else {
                            results[*i] = (
                                200,
                                format!(
                                    "{{\"liked\":true,\"already_liked\":true,\"post_id\":{}}}",
                                    pair[1]
                                )
                                .into_bytes(),
                            );
                        }
                    }
                }
            }
        }
        if batch_ok {
            committed = db.commit().is_ok();
        }
        if !committed {
            let _ = db.rollback();
        }
    }

    (results, committed)
}
