use std::mem::MaybeUninit;

pub(crate) const HEAD_ROOM: usize = 113;
pub(crate) const HEADER_MAX: usize = 65_536;
pub(crate) const BODY_MAX: usize = 65_536;
pub(crate) const NEED_PARTIAL: u32 = 131_072;
pub(crate) const MAX_HEADERS: usize = 64;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Parse {
    Incomplete { need: u32 },
    Continue { request: Request, need: u32 },
    Complete { request: Request, consumed: usize },
    Error { close: bool },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct Request {
    pub(crate) route: Route,
    pub(crate) authorization: Option<Span>,
    pub(crate) body: Body,
    pub(crate) close: bool,
    pub(crate) version: Version,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Route {
    Health,
    Feed,
    Post(i64),
    Create,
    Like(i64),
    NotFound,
    InvalidId { auth: bool },
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Body {
    Empty,
    Input(Span),
    Chunked { at: u32, len: u32 },
    Pending,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct Span {
    pub(crate) start: u32,
    pub(crate) len: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Version {
    Http10,
    Http11,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Status {
    Ok,
    Created,
    BadRequest,
    Unauthorized,
    NotFound,
    Internal,
    Unavailable,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum ErrorBody {
    NotFound,
    InvalidPostId,
    PostNotFound,
    MissingBearer,
    InvalidToken,
    InvalidPayload,
    MalformedJson,
    BodyRequired,
    BodyTooLong,
    Internal,
}

impl Status {
    pub(crate) const fn code(self) -> u16 {
        match self {
            Self::Ok => 200,
            Self::Created => 201,
            Self::BadRequest => 400,
            Self::Unauthorized => 401,
            Self::NotFound => 404,
            Self::Internal => 500,
            Self::Unavailable => 503,
        }
    }
    const fn line(self) -> &'static [u8] {
        match self {
            Self::Ok => b"HTTP/1.1 200 OK",
            Self::Created => b"HTTP/1.1 201 Created",
            Self::BadRequest => b"HTTP/1.1 400 Bad Request",
            Self::Unauthorized => b"HTTP/1.1 401 Unauthorized",
            Self::NotFound => b"HTTP/1.1 404 Not Found",
            Self::Internal => b"HTTP/1.1 500 Internal Server Error",
            Self::Unavailable => b"HTTP/1.1 503 Service Unavailable",
        }
    }
}
impl ErrorBody {
    pub(crate) const fn status(self) -> Status {
        match self {
            Self::NotFound | Self::PostNotFound => Status::NotFound,
            Self::MissingBearer | Self::InvalidToken | Self::InvalidPayload => Status::Unauthorized,
            Self::InvalidPostId | Self::MalformedJson | Self::BodyRequired | Self::BodyTooLong => {
                Status::BadRequest
            }
            Self::Internal => Status::Internal,
        }
    }
    const fn message(self) -> &'static [u8] {
        match self {
            Self::NotFound => b"not found",
            Self::InvalidPostId => b"invalid post id",
            Self::PostNotFound => b"post not found",
            Self::MissingBearer => b"missing bearer token",
            Self::InvalidToken => b"invalid or expired token",
            Self::InvalidPayload => b"invalid token payload",
            Self::MalformedJson => b"malformed JSON body",
            Self::BodyRequired => b"body is required",
            Self::BodyTooLong => b"body must be at most 500 characters",
            Self::Internal => b"internal server error",
        }
    }
}

pub(crate) fn parse(input: &[u8], body: &mut Vec<u8>) -> Parse {
    body.clear();
    // The header slots start uninitialized: httparse writes each slot it fills, and parsed.headers
    // covers exactly those, so no 64-slot array is cleared per request.
    let mut headers = [const { MaybeUninit::<httparse::Header<'_>>::uninit() }; MAX_HEADERS];
    let mut parsed = httparse::Request::new(&mut []);
    let window = input
        .get(..input.len().min(HEADER_MAX + 1))
        .expect("the head window ends at most at input.len()");
    let head = match parsed.parse_with_uninit_headers(window, &mut headers) {
        Ok(httparse::Status::Complete(head)) if head <= HEADER_MAX => head,
        Ok(httparse::Status::Partial) if input.len() <= HEADER_MAX => {
            return Parse::Incomplete { need: NEED_PARTIAL };
        }
        _ => return Parse::Error { close: true },
    };
    let version = match parsed.version {
        Some(0) => Version::Http10,
        Some(1) => Version::Http11,
        _ => return Parse::Error { close: true },
    };
    let (Some(method), Some(path)) = (parsed.method, parsed.path) else {
        return Parse::Error { close: true };
    };
    let mut request = Request {
        route: route(method.as_bytes(), path.as_bytes()),
        authorization: None,
        body: Body::Pending,
        close: version == Version::Http10,
        version,
    };
    let mut length = None;
    let mut chunked = false;
    let mut expect = false;
    for header in parsed.headers {
        let value = trim(header.value);
        if header.name.eq_ignore_ascii_case("authorization") {
            if request.authorization.is_some() {
                return Parse::Error { close: true };
            }
            let start = value.as_ptr().addr() - input.as_ptr().addr();
            let (Ok(start), Ok(len)) = (u32::try_from(start), u32::try_from(value.len())) else {
                return Parse::Error { close: true };
            };
            request.authorization = Some(Span { start, len });
        } else if header.name.eq_ignore_ascii_case("content-length") {
            let Some(n) = content_length(value) else {
                return Parse::Error { close: true };
            };
            if length.is_some_and(|old| old != n) {
                return Parse::Error { close: true };
            }
            length = Some(n);
        } else if header.name.eq_ignore_ascii_case("transfer-encoding") {
            if chunked || !value.eq_ignore_ascii_case(b"chunked") {
                return Parse::Error { close: true };
            }
            chunked = true;
        } else if header.name.eq_ignore_ascii_case("connection") {
            request.close |= value
                .split(|&b| b == b',')
                .any(|part| trim(part).eq_ignore_ascii_case(b"close"));
        } else if header.name.eq_ignore_ascii_case("expect") {
            if !value.eq_ignore_ascii_case(b"100-continue") {
                return Parse::Error { close: true };
            }
            expect = version == Version::Http11;
        }
    }
    if chunked && length.is_some() {
        return Parse::Error { close: true };
    }
    let need;
    if chunked {
        let Some(raw) = input.get(head..) else {
            return Parse::Error { close: true };
        };
        match decode_chunked(raw, body) {
            Chunked::Complete(raw) => {
                request.body = Body::Chunked {
                    at: u32::try_from(head).expect("bounded head"),
                    len: u32::try_from(body.len()).expect("bounded body"),
                };
                return Parse::Complete {
                    request,
                    consumed: head + raw,
                };
            }
            Chunked::Error => return Parse::Error { close: true },
            Chunked::Incomplete => need = head + BODY_MAX,
        }
    } else {
        let n = length.unwrap_or(0);
        need = head + n;
        if input.len() >= need {
            request.body = if n == 0 {
                Body::Empty
            } else {
                Body::Input(Span {
                    start: u32::try_from(head).expect("bounded head"),
                    len: u32::try_from(n).expect("bounded body"),
                })
            };
            return Parse::Complete {
                request,
                consumed: need,
            };
        }
    }
    let need = u32::try_from(need).expect("bounded request");
    if expect {
        Parse::Continue { request, need }
    } else {
        Parse::Incomplete { need }
    }
}

fn trim(mut value: &[u8]) -> &[u8] {
    while let Some((&(b' ' | b'\t'), rest)) = value.split_first() {
        value = rest;
    }
    while let Some((&(b' ' | b'\t'), rest)) = value.split_last() {
        value = rest;
    }
    value
}
fn content_length(bytes: &[u8]) -> Option<usize> {
    if bytes.is_empty() {
        return None;
    }
    bytes.iter().try_fold(0_usize, |n, &b| {
        if !b.is_ascii_digit() {
            return None;
        }
        n.checked_mul(10)?
            .checked_add(usize::from(b - b'0'))
            .filter(|&n| n <= BODY_MAX)
    })
}
fn positive(bytes: &[u8]) -> Option<i64> {
    if bytes.is_empty() {
        return None;
    }
    bytes
        .iter()
        .try_fold(0_i64, |n, &b| {
            if !b.is_ascii_digit() {
                return None;
            }
            n.checked_mul(10)?.checked_add(i64::from(b - b'0'))
        })
        .filter(|&n| n > 0)
}
fn route(method: &[u8], target: &[u8]) -> Route {
    let end = target
        .iter()
        .position(|&b| b == b'?')
        .unwrap_or(target.len());
    let path = target
        .get(..end)
        .expect("end is the first ? position or target.len()");
    match method {
        b"GET" => match path {
            b"/health" => Route::Health,
            b"/feed" => Route::Feed,
            _ => match path.strip_prefix(b"/posts/") {
                Some(id) if !id.contains(&b'/') => positive(id)
                    .map(Route::Post)
                    .unwrap_or(Route::InvalidId { auth: false }),
                _ => Route::NotFound,
            },
        },
        b"POST" if path == b"/posts" => Route::Create,
        b"POST" if path.len() >= 12 && path.starts_with(b"/posts/") && path.ends_with(b"/like") => {
            let id = path.get(7..path.len() - 5).expect(
                "a path of at least 12 bytes that starts with /posts/ and ends with /like holds both",
            );
            if id.contains(&b'/') {
                Route::NotFound
            } else {
                positive(id)
                    .map(Route::Like)
                    .unwrap_or(Route::InvalidId { auth: true })
            }
        }
        _ => Route::NotFound,
    }
}

enum Chunked {
    Complete(usize),
    Incomplete,
    Error,
}
fn crlf(bytes: &[u8], pos: usize) -> Option<usize> {
    bytes
        .get(pos..)?
        .windows(2)
        .position(|part| part == b"\r\n")
        .map(|at| pos + at)
}
fn decode_chunked(raw: &[u8], body: &mut Vec<u8>) -> Chunked {
    let limit = raw.len().min(BODY_MAX);
    let window = raw.get(..limit).expect("limit is at most raw.len()");
    let mut pos = 0;
    loop {
        let Some(end) = crlf(window, pos) else {
            return if raw.len() >= BODY_MAX {
                Chunked::Error
            } else {
                Chunked::Incomplete
            };
        };
        let Some(line) = window.get(pos..end) else {
            return Chunked::Error;
        };
        let semicolon = line.iter().position(|&b| b == b';').unwrap_or(line.len());
        let Some(size) = line.get(..semicolon) else {
            return Chunked::Error;
        };
        if size.is_empty() || size.len() > 8 {
            return Chunked::Error;
        }
        let mut n = 0_usize;
        for &byte in size {
            let digit = match byte {
                b'0'..=b'9' => byte - b'0',
                b'a'..=b'f' => byte - b'a' + 10,
                b'A'..=b'F' => byte - b'A' + 10,
                _ => return Chunked::Error,
            };
            let Some(next) = n
                .checked_mul(16)
                .and_then(|n| n.checked_add(usize::from(digit)))
            else {
                return Chunked::Error;
            };
            n = next;
        }
        if n > BODY_MAX - body.len() {
            return Chunked::Error;
        }
        pos = end + 2;
        if n == 0 {
            loop {
                let Some(end) = crlf(window, pos) else {
                    return if raw.len() >= BODY_MAX {
                        Chunked::Error
                    } else {
                        Chunked::Incomplete
                    };
                };
                if pos == end {
                    return Chunked::Complete(pos + 2);
                }
                pos = end + 2;
            }
        }
        let Some(end) = pos.checked_add(n).and_then(|n| n.checked_add(2)) else {
            return Chunked::Error;
        };
        if end > limit {
            return if end > BODY_MAX {
                Chunked::Error
            } else {
                Chunked::Incomplete
            };
        }
        if raw.get(end - 2..end) != Some(b"\r\n".as_slice()) {
            return Chunked::Error;
        }
        let Some(data) = raw.get(pos..end - 2) else {
            return Chunked::Error;
        };
        let needed = body.len() + n;
        if body.capacity() < needed {
            body.reserve_exact(n);
        }
        body.extend_from_slice(data);
        pos = end;
    }
}

/// # Panics
/// Panics if body_start leaves less than HEAD_ROOM or describes a body outside
/// out or longer than 131,072 bytes.
pub(crate) fn write_head(out: &mut [u8], body_start: usize, status: Status, close: bool) -> usize {
    assert!(body_start >= HEAD_ROOM, "head room too small");
    let length = out
        .len()
        .checked_sub(body_start)
        .expect("reply body too long");
    assert!(length <= 131_072, "reply body too long");
    let mut head = [0; HEAD_ROOM];
    let mut at = 0;
    let mut append = |bytes: &[u8]| {
        head.get_mut(at..at + bytes.len())
            .expect("the head is at most HEAD_ROOM bytes")
            .copy_from_slice(bytes);
        at += bytes.len();
    };
    append(status.line());
    append(b"\r\nContent-Type: application/json\r\nContent-Length: ");
    let mut digits = [0; 6];
    let mut n = length;
    let mut start = digits.len();
    loop {
        start -= 1;
        *digits
            .get_mut(start)
            .expect("a length of at most 131,072 has at most 6 digits") =
            b'0' + u8::try_from(n % 10).expect("decimal digit");
        n /= 10;
        if n == 0 {
            break;
        }
    }
    append(
        digits
            .get(start..)
            .expect("a length of at most 131,072 has at most 6 digits"),
    );
    append(b"\r\n");
    if close {
        append(b"Connection: close\r\n");
    }
    append(b"\r\n");
    let start = body_start - at;
    out.get_mut(start..body_start)
        .expect("start is body_start - at, and body_start >= HEAD_ROOM >= at")
        .copy_from_slice(head.get(..at).expect("the head is at most HEAD_ROOM bytes"));
    start
}
pub(crate) fn write_continue(out: &mut Vec<u8>) {
    out.extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
}
pub(crate) fn write_error(out: &mut Vec<u8>, error: ErrorBody) {
    out.extend_from_slice(b"{\"error\":\"");
    out.extend_from_slice(error.message());
    out.extend_from_slice(b"\"}");
}
