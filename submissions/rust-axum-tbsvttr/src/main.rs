use axum::{
    Router,
    body::{Body, to_bytes},
    extract::{Request, State},
    http::{StatusCode, header},
    response::Response,
};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use rusqlite::{Connection, OptionalExtension, params};
use serde::Serialize;
use serde_json::Value;
use sha2::Sha256;
use std::{
    env,
    time::{Instant, SystemTime, UNIX_EPOCH},
};
use tokio::sync::{mpsc, oneshot};

const READ: &str = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id";
type Reply = (u16, Vec<u8>);
struct Job {
    method: String,
    path: String,
    auth: String,
    body: Vec<u8>,
    done: oneshot::Sender<Reply>,
}
struct App {
    db: Connection,
    secret: Vec<u8>,
    started: Instant,
    transaction: bool,
    failed: bool,
}
#[derive(Serialize)]
struct Post {
    id: i64,
    body: String,
    created_at: String,
    author: String,
    like_count: i64,
}
#[derive(Serialize)]
struct Single {
    post: Post,
}
#[derive(Serialize)]
struct Feed {
    posts: Vec<Post>,
}
#[derive(Serialize)]
struct Like {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}
fn reply(status: u16, value: impl Serialize) -> Reply {
    (
        status,
        serde_json::to_vec(&value).expect("serializable response"),
    )
}
fn failure(status: u16, message: &str) -> Reply {
    #[derive(Serialize)]
    struct Error<'a> {
        error: &'a str,
    }
    reply(status, Error { error: message })
}
fn positive(text: &str) -> Option<i64> {
    if text.is_empty() || !text.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    text.parse::<i64>()
        .ok()
        .filter(|n| *n > 0 && *n <= 9_007_199_254_740_991)
}
fn space(c: char) -> bool {
    matches!(c as u32, 9..=13 | 32 | 0xa0 | 0x1680 | 0x2000..=0x200a | 0x2028 | 0x2029 | 0x202f | 0x205f | 0x3000 | 0xfeff)
}
fn row(row: &rusqlite::Row<'_>) -> rusqlite::Result<Post> {
    Ok(Post {
        id: row.get(0)?,
        body: row.get(1)?,
        created_at: row.get(2)?,
        author: row.get(3)?,
        like_count: row.get(4)?,
    })
}
impl App {
    fn authenticate(&self, auth: &str) -> Result<(i64, String), &'static str> {
        let token = auth.strip_prefix("Bearer ").ok_or("missing bearer token")?;
        let invalid = "invalid or expired token";
        let parts: Vec<_> = token.split('.').collect();
        if parts.len() != 3 {
            return Err(invalid);
        }
        let mut decoded = Vec::with_capacity(3);
        for part in &parts {
            if part.is_empty() {
                return Err(invalid);
            }
            decoded.push(URL_SAFE_NO_PAD.decode(part).map_err(|_| invalid)?);
        }
        let mut mac = Hmac::<Sha256>::new_from_slice(&self.secret).map_err(|_| invalid)?;
        mac.update(parts[0].as_bytes());
        mac.update(b".");
        mac.update(parts[1].as_bytes());
        mac.verify_slice(&decoded[2]).map_err(|_| invalid)?;
        let head: Value = serde_json::from_slice(&decoded[0]).map_err(|_| invalid)?;
        let claims: Value = serde_json::from_slice(&decoded[1]).map_err(|_| invalid)?;
        if head["alg"].as_str() != Some("HS256") {
            return Err(invalid);
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| invalid)?
            .as_secs_f64();
        let expiry = claims["exp"].as_f64().ok_or(invalid)?;
        if !expiry.is_finite() || expiry <= now {
            return Err(invalid);
        }
        if let Some(value) = claims.get("nbf") {
            let nbf = value.as_f64().ok_or(invalid)?;
            if !nbf.is_finite() || nbf > now {
                return Err(invalid);
            }
        }
        let payload = "invalid token payload";
        let id = positive(claims["sub"].as_str().ok_or(payload)?).ok_or(payload)?;
        let name = claims["username"].as_str().ok_or(payload)?;
        Ok((id, name.to_owned()))
    }
    fn begin(&mut self) -> rusqlite::Result<()> {
        if !self.transaction {
            self.db.execute_batch("BEGIN IMMEDIATE")?;
            self.transaction = true;
        }
        Ok(())
    }
    fn handle(&mut self, job: &Job) -> rusqlite::Result<Reply> {
        if self.failed {
            return Ok(failure(500, "internal server error"));
        }
        let get = job.method == "GET";
        if get && job.path == "/health" {
            #[derive(Serialize)]
            struct Health {
                status: &'static str,
                db: &'static str,
                uptime_s: u64,
            }
            #[derive(Serialize)]
            struct Degraded {
                status: &'static str,
                db: &'static str,
                error: String,
            }
            return Ok(
                match self
                    .db
                    .prepare_cached("SELECT 1")?
                    .query_row([], |r| r.get::<_, i64>(0))
                {
                    Ok(_) => reply(
                        200,
                        Health {
                            status: "ok",
                            db: "ok",
                            uptime_s: self.started.elapsed().as_secs(),
                        },
                    ),
                    Err(e) => reply(
                        503,
                        Degraded {
                            status: "degraded",
                            db: "unreachable",
                            error: e.to_string(),
                        },
                    ),
                },
            );
        }
        let create = job.method == "POST" && job.path == "/posts";
        let like = job.method == "POST" && job.path.ends_with("/like");
        let feed = get && job.path == "/feed";
        let id_text = job
            .path
            .strip_prefix("/posts/")
            .and_then(|s| {
                if like {
                    s.strip_suffix("/like")
                } else if get {
                    Some(s)
                } else {
                    None
                }
            })
            .filter(|s| !s.is_empty() && !s.contains('/'));
        if !create && !feed && id_text.is_none() {
            return Ok(failure(404, "not found"));
        }
        let (user, name) = if job.method == "POST" {
            match self.authenticate(&job.auth) {
                Ok(claims) => claims,
                Err(message) => return Ok(failure(401, message)),
            }
        } else {
            (0, String::new())
        };
        let id = positive(id_text.unwrap_or("")).unwrap_or(0);
        if !create && !feed && id == 0 {
            return Ok(failure(400, "invalid post id"));
        }
        if create {
            let value: Value = match serde_json::from_slice(&job.body) {
                Ok(value) => value,
                Err(_) => return Ok(failure(400, "malformed JSON body")),
            };
            let body = value["body"].as_str().unwrap_or("").trim_matches(space);
            if body.is_empty() {
                return Ok(failure(400, "body is required"));
            }
            if body.chars().count() > 500 {
                return Ok(failure(400, "body must be at most 500 characters"));
            }
            self.begin()?;
            let mut statement = self.db.prepare_cached(
                "INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at",
            )?;
            let mut rows = statement.query(params![user, body])?;
            let result = rows.next()?.ok_or(rusqlite::Error::QueryReturnedNoRows)?;
            let post = Post {
                id: result.get(0)?,
                body: body.to_owned(),
                created_at: result.get(1)?,
                author: name,
                like_count: 0,
            };
            // Finish RETURNING before committing; propagate errors after the first row.
            rows.next()?;
            return Ok(reply(201, Single { post }));
        }
        if like {
            self.begin()?;
            let changed = self.db.prepare_cached("INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING")?.execute(params![user, id])?;
            if changed == 0
                && self
                    .db
                    .prepare_cached("SELECT 1 FROM posts WHERE id=?")?
                    .query_row([id], |r| r.get::<_, i64>(0))
                    .optional()?
                    .is_none()
            {
                return Ok(failure(404, "post not found"));
            }
            return Ok(reply(
                if changed == 0 { 200 } else { 201 },
                Like {
                    liked: true,
                    already_liked: changed == 0,
                    post_id: id,
                },
            ));
        }
        if feed {
            let mut statement = self.db.prepare_cached(&format!(
                "{READ} ORDER BY p.created_at DESC,p.id DESC LIMIT 20"
            ))?;
            let posts = statement
                .query_map([], row)?
                .collect::<rusqlite::Result<Vec<_>>>()?;
            Ok(reply(200, Feed { posts }))
        } else {
            let post = self
                .db
                .prepare_cached(&format!("{READ} WHERE p.id=?"))?
                .query_row([id], row)
                .optional()?;
            Ok(match post {
                Some(post) => reply(200, Single { post }),
                None => failure(404, "post not found"),
            })
        }
    }
    async fn work(mut self, mut queue: mpsc::Receiver<Job>) {
        let mut replies = Vec::with_capacity(256);
        while let Some(first) = queue.recv().await {
            tokio::task::yield_now().await; // Batch ready requests without a timer.
            let mut next = Some(first);
            while let Some(job) = next {
                let result = self
                    .handle(&job)
                    .unwrap_or_else(|_| failure(500, "internal server error"));
                if self.transaction && self.db.is_autocommit() {
                    self.failed = true;
                }
                replies.push((job.done, result));
                next = if replies.len() < 256 {
                    queue.try_recv().ok()
                } else {
                    None
                };
            }
            if self.transaction {
                if !self.failed {
                    self.failed = self.db.execute_batch("COMMIT").is_err();
                }
                if self.failed {
                    let _ = self.db.execute_batch("ROLLBACK");
                    assert!(self.db.is_autocommit(), "unable to roll back failed batch");
                }
            }
            for (done, result) in replies.drain(..) {
                let _ = done.send(if self.failed {
                    failure(500, "internal server error")
                } else {
                    result
                });
            }
            self.transaction = false;
            self.failed = false;
        }
    }
}
fn http_response((status, data): Reply) -> Response {
    Response::builder()
        .status(StatusCode::from_u16(status).unwrap())
        .header(header::CONTENT_TYPE, "application/json")
        .header(header::CONTENT_LENGTH, data.len())
        .body(Body::from(data))
        .unwrap()
}
async fn handle(State(queue): State<mpsc::Sender<Job>>, request: Request) -> Response {
    let (parts, body) = request.into_parts();
    let data = match to_bytes(body, 16384).await {
        Ok(data) => data,
        Err(_) => return http_response(failure(413, "request body too large")),
    };
    let (done, result) = oneshot::channel();
    let job = Job {
        method: parts.method.to_string(),
        path: parts.uri.path().to_owned(),
        auth: parts
            .headers
            .get(header::AUTHORIZATION)
            .and_then(|v| v.to_str().ok())
            .unwrap_or("")
            .to_owned(),
        body: data.to_vec(),
        done,
    };
    if queue.send(job).await.is_err() {
        return http_response(failure(500, "internal server error"));
    }
    http_response(
        result
            .await
            .unwrap_or_else(|_| failure(500, "internal server error")),
    )
}
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = env::var("SQLITE_PATH")?;
    let secret = env::var("JWT_SECRET")?;
    if path.is_empty() || secret.is_empty() {
        return Err("SQLITE_PATH and JWT_SECRET are required".into());
    }
    let db = Connection::open_with_flags(
        path,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )?;
    db.busy_timeout(std::time::Duration::ZERO)?;
    db.execute_batch("PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA cache_size=-8192; PRAGMA mmap_size=268435456;")?;
    db.set_prepared_statement_cache_capacity(16);
    let app = App {
        db,
        secret: secret.into_bytes(),
        started: Instant::now(),
        transaction: false,
        failed: false,
    };
    let (send, receive) = mpsc::channel(1024);
    tokio::spawn(app.work(receive));
    let listener = tokio::net::TcpListener::bind((
        env::var("HOST").unwrap_or("127.0.0.1".into()),
        env::var("PORT").unwrap_or("3000".into()).parse::<u16>()?,
    ))
    .await?;
    axum::serve(listener, Router::new().fallback(handle).with_state(send)).await?;
    Ok(())
}
