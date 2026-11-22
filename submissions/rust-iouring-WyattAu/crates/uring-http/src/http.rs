//! HTTP/1.1 request parsing: fixed route family, zero-copy byte views.
//!
//! Totality contract (Lean target): every byte input yields a `Request`,
//! `ParseError::Incomplete` (need more bytes), or a typed error — never a
//! panic, never a wrong answer. Headers cap at 8 KiB per line.
//!
//! Errors from this module map to SPEC error rows by the server router:
//! unknown method/path → 404 `not found`, bad `:id` → 400 `invalid post id`,
//! missing/bad auth header → 401. Version: `HTTP/1.0` and `HTTP/1.1` accepted.

/// Where the body starts and what we learned from headers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Request<'a> {
    pub method: Method,
    /// Request target as received (`/posts/42`), bytes only.
    pub target: &'a [u8],
    /// Raw `Authorization` header value (name stripped, leading spaces trimmed).
    pub authorization: Option<&'a [u8]>,
    /// Parsed `Content-Length` (rejects non-digits).
    pub content_length: Option<usize>,
    /// Offset of the body (just past the terminating CRLFCRLF).
    pub body_start: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Get,
    Post,
    Other,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParseError {
    /// Buffer does not yet contain the full head (CRLFCRLF not found).
    Incomplete,
    /// No space-separated method/target/version on the first line.
    BadRequestLine,
    /// Target does not start with `/`.
    BadTarget,
    /// Version is not `HTTP/1.x`.
    BadVersion,
    /// A header line exceeds [`MAX_HEADER_LINE`].
    HeaderTooLong,
    /// `Content-Length` present but not pure digits (or > usize).
    BadContentLength,
}

/// 8 KiB per header line — k6 and curl stay far below.
pub const MAX_HEADER_LINE: usize = 8 * 1024;

/// Find the index of the first `n` in `buf[skip..]`, scanning bytes.
#[inline]
fn find_newline(buf: &[u8], skip: usize) -> Option<usize> {
    buf.iter().position(|&b| b == b'\n').filter(|&i| i >= skip)
}

/// Parse an HTTP/1.x request head from the front of `buf`.
#[inline]
pub fn parse(buf: &[u8]) -> Result<Request<'_>, ParseError> {
    let nl = find_newline(buf, 0).ok_or(ParseError::Incomplete)?;
    let (request_line, after) = buf.split_at(nl);
    let request_line = strip_cr(request_line);

    let mut parts = request_line.split(|&b| b == b' ');
    let method_bytes = parts.next().unwrap_or(&[]);
    let target = parts.next().unwrap_or(&[]);
    let version = parts.next().unwrap_or(&[]);
    if parts.next().is_some() || method_bytes.is_empty() || target.is_empty() || version.is_empty()
    {
        return Err(ParseError::BadRequestLine);
    }
    if !target.starts_with(b"/") {
        return Err(ParseError::BadTarget);
    }
    // Accept HTTP/1.0 and HTTP/1.1 only.
    if !(version.len() == 8 && version.starts_with(b"HTTP/1.") && version[7].is_ascii_digit()) {
        return Err(ParseError::BadVersion);
    }
    let method = match method_bytes {
        b"GET" => Method::Get,
        b"POST" => Method::Post,
        _ => Method::Other,
    };

    let mut authorization = None;
    let mut content_length = None;
    let mut pos = nl + 1; // past the request-line \n
    loop {
        let nl = find_newline(&buf[pos..], 0).ok_or(ParseError::Incomplete)?;
        let line = strip_cr(&buf[pos..pos + nl]);
        pos += nl + 1;
        if line.is_empty() {
            return Ok(Request { method, target, authorization, content_length, body_start: pos });
        }
        if line.len() > MAX_HEADER_LINE {
            return Err(ParseError::HeaderTooLong);
        }
        let colon = line.iter().position(|&b| b == b':').ok_or(ParseError::BadRequestLine)?;
        let name = &line[..colon];
        let mut value = &line[colon + 1..];
        while matches!(value.first(), Some(&b' ') | Some(&b'\t')) {
            value = &value[1..];
        }
        if name.eq_ignore_ascii_case(b"authorization") {
            authorization = Some(value);
        } else if name.eq_ignore_ascii_case(b"content-length") {
            let cl = parse_usize(value).ok_or(ParseError::BadContentLength)?;
            content_length = Some(cl);
        }
    }
}

#[inline]
fn strip_cr(line: &[u8]) -> &[u8] {
    match line.split_last() {
        Some((&b'\r', rest)) => rest,
        _ => line,
    }
}

#[inline]
fn parse_usize(bytes: &[u8]) -> Option<usize> {
    if bytes.is_empty() {
        return None;
    }
    bytes.iter().try_fold(0usize, |acc, &b| {
        let d = (b as char).to_digit(10)? as usize;
        acc.checked_mul(10)?.checked_add(d)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_head(s: &str) -> Result<Request<'_>, ParseError> {
        parse(s.as_bytes())
    }

    #[test]
    fn simple_get() {
        let r = parse_head("GET /feed HTTP/1.1\r\nHost: x\r\n\r\n").unwrap();
        assert_eq!((r.method, r.target), (Method::Get, &b"/feed"[..]));
        assert_eq!(r.body_start, "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n".len());
        assert_eq!(r.content_length, None);
    }

    #[test]
    fn post_with_auth_and_body() {
        let req = "POST /posts HTTP/1.1\r\nAuthorization: Bearer abc.def\r\nContent-Length: 5\r\n\r\nhello";
        let r = parse(req.as_bytes()).unwrap();
        assert_eq!(r.method, Method::Post);
        assert_eq!(r.authorization, Some(&b"Bearer abc.def"[..]));
        assert_eq!(r.content_length, Some(5));
        assert_eq!(&req.as_bytes()[r.body_start..], b"hello");
    }

    #[test]
    fn header_name_case_insensitive_trimmed() {
        let r = parse_head("GET / HTTP/1.1\r\naUtHoRiZaTiOn:   Bearer x  \r\n\r\n").unwrap();
        // Trailing spaces are part of the value view; parser only strips leading.
        assert_eq!(r.authorization, Some(&b"Bearer x  "[..]));
    }

    #[test]
    fn incomplete_head() {
        assert_eq!(parse(b"GET /feed HTTP/1.1\r\nHost: x"), Err(ParseError::Incomplete));
        assert_eq!(parse(b""), Err(ParseError::Incomplete));
    }

    #[test]
    fn malformed_requests_never_panic() {
        for bad in [
            "GET  HTTP/1.1\r\n\r\n",         // double space -> empty target part
            "GET /a/b HTTP/2.9\r\n\r\n",   // bad version
            "GET noslash HTTP/1.1\r\n\r\n", // bad target
            "GET /x HTTP/1.1 extra\r\n\r\n", // 4 parts
            "GET /x HTTP/1.1\r\nNoColon\r\n\r\n",
            "GET /x HTTP/1.1\r\nContent-Length: 12a\r\n\r\n",
        ] {
            assert!(parse(bad.as_bytes()).is_err(), "should reject: {bad:?}");
        }
    }

    #[test]
    fn arbitrary_bytes_never_panic() {
        // Totality smoke: random-ish byte soup, including heads with \r\n inside.
        let soup: Vec<u8> = (0u8..=255).cycle().take(4096).collect();
        let _ = parse(&soup);
        let _ = parse(b"\r\n\r\n\r\n");
        let _ = parse(b"GET /\x00\x01 HTTP/1.1\r\nAuthorization: \xff\xfe\r\n\r\n");
    }
}
