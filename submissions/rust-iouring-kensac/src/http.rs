//! HTTP/1.1 request framing. Only the parts the API uses are looked at: the request line,
//! Authorization and Content-Length. One pass over the lines finds them and the end of the head.

pub struct Request<'a> {
    pub method: &'a [u8],
    pub path: &'a [u8],
    pub auth: Option<&'a [u8]>,
    pub body: &'a [u8],
}

pub enum Parsed<'a> {
    /// A whole request and the bytes it took.
    Done(Request<'a>, usize),
    Partial,
    Bad,
}

const MAX_HEAD: usize = 16 * 1024;
const MAX_BODY: usize = 1024 * 1024;

#[inline]
fn trim(mut s: &[u8]) -> &[u8] {
    while let [b' ' | b'\t', rest @ ..] = s {
        s = rest;
    }
    while let [rest @ .., b' ' | b'\t'] = s {
        s = rest;
    }
    s
}

#[inline]
fn eq_ignore_case(a: &[u8], lower: &[u8]) -> bool {
    a.len() == lower.len() && a.iter().zip(lower).all(|(x, y)| x.to_ascii_lowercase() == *y)
}

pub fn parse(buf: &[u8]) -> Parsed<'_> {
    let partial = || if buf.len() > MAX_HEAD { Parsed::Bad } else { Parsed::Partial };
    // Request line.
    let Some(nl) = memchr::memchr(b'\n', buf) else { return partial() };
    let line = &buf[..nl];
    let line = line.strip_suffix(b"\r").unwrap_or(line);
    let Some(sp1) = memchr::memchr(b' ', line) else { return Parsed::Bad };
    let method = &line[..sp1];
    let rest = &line[sp1 + 1..];
    let Some(sp2) = memchr::memchr(b' ', rest) else { return Parsed::Bad };
    let target = &rest[..sp2];
    if !rest[sp2 + 1..].starts_with(b"HTTP/1.") || method.is_empty() || target.is_empty() {
        return Parsed::Bad;
    }
    let path = match memchr::memchr(b'?', target) {
        Some(q) => &target[..q],
        None => target,
    };
    // Headers, up to the empty line.
    let mut auth = None;
    let mut len = 0usize;
    let mut pos = nl + 1;
    loop {
        let Some(n) = memchr::memchr(b'\n', &buf[pos..]) else { return partial() };
        let l = &buf[pos..pos + n];
        pos += n + 1;
        let l = l.strip_suffix(b"\r").unwrap_or(l);
        if l.is_empty() {
            break;
        }
        let Some(colon) = memchr::memchr(b':', l) else { continue };
        let name = &l[..colon];
        match name.len() {
            13 if eq_ignore_case(name, b"authorization") => auth = Some(trim(&l[colon + 1..])),
            14 if eq_ignore_case(name, b"content-length") => {
                let value = trim(&l[colon + 1..]);
                if value.is_empty() || value.len() > 9 || !value.iter().all(u8::is_ascii_digit) {
                    return Parsed::Bad;
                }
                len = value.iter().fold(0, |a, &c| a * 10 + (c - b'0') as usize);
                if len > MAX_BODY {
                    return Parsed::Bad;
                }
            }
            17 if eq_ignore_case(name, b"transfer-encoding") => return Parsed::Bad,
            _ => {}
        }
    }
    if buf.len() < pos + len {
        return Parsed::Partial;
    }
    Parsed::Done(Request { method, path, auth, body: &buf[pos..pos + len] }, pos + len)
}
