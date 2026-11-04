use std::env;
use std::io;
use std::io::Write as _;
use std::sync::mpsc;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

mod checkpoint;
mod db;
mod reader;
mod stats;
mod uring;
mod writer;
use db::ReadDb;

use base64::engine::general_purpose::{URL_SAFE, URL_SAFE_NO_PAD};
use base64::Engine;
use hmac::{Hmac, Mac};
use sha2::Sha256;

struct Request<'a> {
    method: &'a str,
    path: &'a str,
    auth: Option<&'a str>,
    body: &'a [u8],
    close: bool,
}

struct Claims {
    user_id: i64,
    username: String,
}

enum ParseResult<'a> {
    Incomplete,
    Bad,
    Complete(Request<'a>, usize),
}

fn main() -> io::Result<()> {
    let host = env::var("HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
    let port = env::var("PORT").unwrap_or_else(|_| "3000".to_string());
    let db_path = env::var("SQLITE_PATH").expect("SQLITE_PATH");
    let secret = env::var("JWT_SECRET").expect("JWT_SECRET").into_bytes();

    let coalesce = env::var("READ_COALESCE").map(|v| v != "0").unwrap_or(true);
    let (job_tx, job_rx) = mpsc::channel();
    let (res_tx, res_rx) = mpsc::channel();
    let (ready_tx, ready_rx) = mpsc::channel();
    let event_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if event_fd < 0 {
        return Err(io::Error::last_os_error());
    }
    writer::spawn(db_path.clone(), job_rx, res_tx, event_fd, ready_tx);
    let _ = ready_rx.recv();
    checkpoint::spawn(db_path.clone());
    let (read_tx, read_rx) = mpsc::channel();
    let (read_res_tx, read_res_rx) = mpsc::channel();
    let read_event_fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
    if read_event_fd < 0 {
        return Err(io::Error::last_os_error());
    }
    reader::spawn(db_path.clone(), read_rx, read_res_tx, read_event_fd);
    let read = ReadDb::open(&db_path).map_err(|e| io::Error::new(io::ErrorKind::Other, e))?;
    let app = App {
        read,
        coalesce,
        secret,
        started: Instant::now(),
    };
    let addr = format!("{host}:{port}");
    uring::run(app, addr, job_tx, res_rx, event_fd, read_tx, read_res_rx, read_event_fd)
}

pub(crate) struct App {
    pub read: ReadDb,
    coalesce: bool,
    secret: Vec<u8>,
    started: Instant,
}

pub(crate) enum Handled {
    Reply(u16, Vec<u8>),
    Feed,
    Post(i64),
    Write(WriteCmd),
}

pub(crate) struct WriteCmd {
    pub kind: WriteKind,
}

pub(crate) enum WriteKind {
    Create {
        user_id: i64,
        username: String,
        body: String,
    },
    Like {
        user_id: i64,
        post_id: i64,
    },
}

impl App {
    pub(crate) fn handle(&mut self, req: &Request<'_>) -> Handled {
        match (req.method, req.path) {
            ("GET", "/health") => {
                if self.read.health() {
                    Handled::Reply(
                        200,
                        format!(
                            "{{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":{}}}",
                            self.started.elapsed().as_secs()
                        )
                        .into_bytes(),
                    )
                } else {
                    Handled::Reply(
                        503,
                        b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database error\"}".to_vec(),
                    )
                }
            }
            ("GET", "/feed") => {
                if self.coalesce {
                    Handled::Feed
                } else {
                    match self.read.feed() {
                        Ok(body) => Handled::Reply(200, body),
                        Err(_) => Handled::Reply(500, error("internal server error")),
                    }
                }
            }
            ("POST", "/posts") => match authenticate(req, &self.secret) {
                Err(resp) => Handled::Reply(resp.0, resp.1),
                Ok(claims) => {
                    let value: serde_json::Value = match serde_json::from_slice(&req.body) {
                        Ok(v) => v,
                        Err(_) => return Handled::Reply(400, error("malformed JSON body")),
                    };
                    let Some(body) = value.get("body").and_then(serde_json::Value::as_str) else {
                        return Handled::Reply(400, error("body is required"));
                    };
                    let body = body.trim();
                    if body.is_empty() {
                        return Handled::Reply(400, error("body is required"));
                    }
                    if body.chars().count() > 500 {
                        return Handled::Reply(400, error("body must be at most 500 characters"));
                    }
                    Handled::Write(WriteCmd {
                        kind: WriteKind::Create {
                            user_id: claims.user_id,
                            username: claims.username,
                            body: body.to_string(),
                        },
                    })
                }
            },
            ("GET", path) if path.starts_with("/posts/") => {
                let Some(id) = parse_positive_id(&path["/posts/".len()..]) else {
                    return Handled::Reply(400, error("invalid post id"));
                };
                if self.coalesce {
                    Handled::Post(id)
                } else {
                    match self.read.post(id) {
                        Ok(Some(body)) => Handled::Reply(200, body),
                        Ok(None) => Handled::Reply(404, error("post not found")),
                        Err(_) => Handled::Reply(500, error("internal server error")),
                    }
                }
            }
            ("POST", path) if path.starts_with("/posts/") && path.ends_with("/like") => {
                match authenticate(req, &self.secret) {
                    Err(resp) => Handled::Reply(resp.0, resp.1),
                    Ok(claims) => {
                        let rest = &path["/posts/".len()..path.len() - "/like".len()];
                        let Some(id) = parse_positive_id(rest) else {
                            return Handled::Reply(400, error("invalid post id"));
                        };
                        Handled::Write(WriteCmd {
                            kind: WriteKind::Like {
                                user_id: claims.user_id,
                                post_id: id,
                            },
                        })
                    }
                }
            }
            _ => Handled::Reply(404, error("not found")),
        }
    }
}

pub(crate) fn apply_write(write: &db::WriteDb, cmd: &WriteCmd) -> (u16, Vec<u8>) {
    match &cmd.kind {
        WriteKind::Create {
            user_id,
            username,
            body,
        } => match write.create_post(*user_id, body) {
            Ok((id, created_at)) => {
                let body_json = json_string(body);
                let author_json = json_string(username);
                let created_json = json_string(&created_at);
                (
                    201,
                    format!(
                        "{{\"post\":{{\"id\":{id},\"body\":{body_json},\"created_at\":{created_json},\"author\":{author_json},\"like_count\":0}}}}"
                    )
                    .into_bytes(),
                )
            }
            Err(e) => {
                eprintln!("create error: {e}");
                (500, error("internal server error"))
            }
        },
        WriteKind::Like { user_id, post_id } => match write.like(*user_id, *post_id) {
            Ok(Some((inserted, _count))) => {
                let status = if inserted { 201 } else { 200 };
                (
                    status,
                    format!(
                        "{{\"liked\":true,\"already_liked\":{},\"post_id\":{post_id}}}",
                        !inserted
                    )
                    .into_bytes(),
                )
            }
            Ok(None) => (404, error("post not found")),
            Err(_) => (500, error("internal server error")),
        },
    }
}

fn parse_request<'a>(buf: &'a [u8]) -> ParseResult<'a> {
    let Some(head_end) = find_head_end(buf) else {
        return ParseResult::Incomplete;
    };
    let head = &buf[..head_end];
    let mut lines = head.split(|&b| b == b'\n');
    let Some(line) = lines.next() else {
        return ParseResult::Bad;
    };
    let line = trim_cr(line);
    let mut parts = line.split(|&b| b == b' ');
    let (Some(method), Some(target), Some(version)) = (parts.next(), parts.next(), parts.next()) else {
        return ParseResult::Bad;
    };
    if parts.next().is_some() || method.is_empty() || target.is_empty() {
        return ParseResult::Bad;
    }

    let mut content_length = 0usize;
    let mut auth: Option<&str> = None;
    let mut close = version == b"HTTP/1.0";
    for raw in lines {
        let line = trim_cr(raw);
        if line.is_empty() {
            continue;
        }
        let Some(colon) = line.iter().position(|&b| b == b':') else {
            return ParseResult::Bad;
        };
        let name = &line[..colon];
        let value = trim_ascii(&line[colon + 1..]);
        if name.eq_ignore_ascii_case(b"content-length") {
            let Ok(text) = std::str::from_utf8(value) else {
                return ParseResult::Bad;
            };
            let Ok(n) = text.parse::<usize>() else {
                return ParseResult::Bad;
            };
            content_length = n;
        } else if name.eq_ignore_ascii_case(b"authorization") {
            if let Ok(text) = std::str::from_utf8(value) {
                auth = Some(text);
            }
        } else if name.eq_ignore_ascii_case(b"connection") {
            if value.eq_ignore_ascii_case(b"close") {
                close = true;
            }
        }
    }

    let total = head_end + content_length;
    if buf.len() < total {
        return ParseResult::Incomplete;
    }
    if content_length > (1 << 20) {
        return ParseResult::Bad;
    }
    let body = &buf[head_end..total];
    let target = std::str::from_utf8(target).unwrap_or("");
    let path = target.split('?').next().unwrap_or("");
    let method = std::str::from_utf8(method).unwrap_or("");
    let req = Request {
        method,
        path,
        auth,
        body,
        close,
    };
    ParseResult::Complete(req, total)
}

fn find_head_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n").map(|p| p + 4)
}

fn trim_cr(line: &[u8]) -> &[u8] {
    if line.last() == Some(&b'\r') {
        &line[..line.len() - 1]
    } else {
        line
    }
}

fn trim_ascii(mut s: &[u8]) -> &[u8] {
    while let Some((&b, rest)) = s.split_first() {
        if b == b' ' || b == b'\t' {
            s = rest;
        } else {
            break;
        }
    }
    while let Some((&b, rest)) = s.split_last() {
        if b == b' ' || b == b'\t' {
            s = rest;
        } else {
            break;
        }
    }
    s
}

pub(crate) fn response_bytes(status: u16, body: &[u8], close: bool) -> Vec<u8> {
    let reason = status_reason(status);
    let mut out = Vec::with_capacity(96 + body.len());
    write!(
        &mut out,
        "HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n",
        body.len()
    )
    .unwrap();
    if close {
        out.extend_from_slice(b"Connection: close\r\n\r\n");
    } else {
        out.extend_from_slice(b"\r\n");
    }
    out.extend_from_slice(body);
    out
}

fn authenticate(req: &Request<'_>, secret: &[u8]) -> Result<Claims, (u16, Vec<u8>)> {
    let Some(value) = req.auth.as_deref() else {
        return Err((401, error("missing bearer token")));
    };
    let Some(token) = value.strip_prefix("Bearer ") else {
        return Err((401, error("missing bearer token")));
    };
    verify_token(token, secret).map_err(|kind| (401, error(kind)))
}

fn verify_token(token: &str, secret: &[u8]) -> Result<Claims, &'static str> {
    let mut parts = token.split('.');
    let (Some(header_b64), Some(payload_b64), Some(sig_b64)) =
        (parts.next(), parts.next(), parts.next())
    else {
        return Err("invalid or expired token");
    };
    if parts.next().is_some() {
        return Err("invalid or expired token");
    }
    let header = decode_b64(header_b64).ok_or("invalid or expired token")?;
    let payload = decode_b64(payload_b64).ok_or("invalid or expired token")?;
    let signature = decode_b64(sig_b64).ok_or("invalid or expired token")?;
    let header: serde_json::Value =
        serde_json::from_slice(&header).map_err(|_| "invalid or expired token")?;
    if header.get("alg").and_then(serde_json::Value::as_str) != Some("HS256") {
        return Err("invalid or expired token");
    }

    let mut mac = Hmac::<Sha256>::new_from_slice(secret).map_err(|_| "invalid or expired token")?;
    mac.update(header_b64.as_bytes());
    mac.update(b".");
    mac.update(payload_b64.as_bytes());
    mac.verify_slice(&signature)
        .map_err(|_| "invalid or expired token")?;

    let payload: serde_json::Value =
        serde_json::from_slice(&payload).map_err(|_| "invalid or expired token")?;
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| "invalid or expired token")?
        .as_secs() as i64;
    let exp = payload
        .get("exp")
        .and_then(serde_json::Value::as_i64)
        .ok_or("invalid or expired token")?;
    if exp <= now {
        return Err("invalid or expired token");
    }
    let sub = payload
        .get("sub")
        .and_then(serde_json::Value::as_str)
        .ok_or("invalid token payload")?;
    let user_id = sub.parse::<i64>().map_err(|_| "invalid token payload")?;
    if user_id <= 0 {
        return Err("invalid token payload");
    }
    let username = payload
        .get("username")
        .and_then(serde_json::Value::as_str)
        .ok_or("invalid token payload")?
        .to_string();
    Ok(Claims { user_id, username })
}

fn decode_b64(s: &str) -> Option<Vec<u8>> {
    URL_SAFE_NO_PAD
        .decode(s)
        .or_else(|_| URL_SAFE.decode(s))
        .ok()
}

fn parse_positive_id(s: &str) -> Option<i64> {
    if s.is_empty() || s.starts_with('+') || s.contains('.') || s.contains('e') || s.contains('E') {
        return None;
    }
    s.parse::<i64>().ok().filter(|v| *v > 0)
}

fn json_string(s: &str) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "\"\"".to_string())
}

fn error(message: &str) -> Vec<u8> {
    format!("{{\"error\":{}}}", json_string(message)).into_bytes()
}

fn status_reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "Internal Server Error",
    }
}
