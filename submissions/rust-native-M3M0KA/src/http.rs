//! HTTP/1.1 request framing and JSON response building.

pub const MAX_HEAD: usize = 16 * 1024;
pub const MAX_BODY: usize = 64 * 1024;

pub struct Request<'a> {
    pub method: &'a [u8],
    pub path: &'a [u8],
    pub auth: Option<&'a [u8]>,
    pub body: &'a [u8],
    pub keep_alive: bool,
    pub expect_continue: bool,
}

fn ci_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len()
        && a.iter()
            .zip(b)
            .all(|(&x, &y)| x.eq_ignore_ascii_case(&y))
}

/// Parse one request from the front of `buf`.
///
/// Returns `Ok(None)` when more bytes are needed, `Ok(Some((req, used)))` on a
/// complete request, and `Err(())` when the input cannot be served.
pub fn parse(buf: &[u8]) -> Result<Option<(Request<'_>, usize)>, ()> {
    let mut head_end = 0usize;
    let mut i = 0usize;
    while i + 3 < buf.len() {
        if buf[i] == b'\r' && buf[i + 1] == b'\n' && buf[i + 2] == b'\r' && buf[i + 3] == b'\n' {
            head_end = i + 4;
            break;
        }
        i += 1;
    }
    if head_end == 0 {
        return if buf.len() > MAX_HEAD { Err(()) } else { Ok(None) };
    }
    if head_end > MAX_HEAD {
        return Err(());
    }

    let mut i = 0usize;
    let ms = i;
    while i < head_end && buf[i] != b' ' {
        i += 1;
    }
    if i >= head_end {
        return Err(());
    }
    let method = &buf[ms..i];
    i += 1;
    let ps = i;
    while i < head_end && buf[i] != b' ' {
        i += 1;
    }
    if i >= head_end {
        return Err(());
    }
    let path = &buf[ps..i];
    while i < head_end && buf[i] != b'\r' && buf[i] != b'\n' {
        i += 1;
    }
    if i + 1 >= head_end {
        return Err(());
    }
    i += 2;

    let mut keep_alive = true;
    let mut auth: Option<&[u8]> = None;
    let mut content_length = 0usize;
    let mut have_length = false;
    let mut expect_continue = false;

    while i < head_end {
        let ls = i;
        while i < head_end && buf[i] != b'\r' && buf[i] != b'\n' {
            i += 1;
        }
        let le = i;
        if i < head_end && buf[i] == b'\r' {
            i += 1;
        }
        if i < head_end && buf[i] == b'\n' {
            i += 1;
        }
        if le == ls {
            break;
        }
        let mut colon = ls;
        while colon < le && buf[colon] != b':' {
            colon += 1;
        }
        if colon == le {
            continue;
        }
        let mut vs = colon + 1;
        while vs < le && (buf[vs] == b' ' || buf[vs] == b'\t') {
            vs += 1;
        }
        let mut ve = le;
        while ve > vs && (buf[ve - 1] == b' ' || buf[ve - 1] == b'\t') {
            ve -= 1;
        }
        let name = &buf[ls..colon];
        let value = &buf[vs..ve];

        if ci_eq(name, b"authorization") {
            auth = Some(value);
        } else if ci_eq(name, b"content-length") {
            if value.is_empty() {
                return Err(());
            }
            let mut v = 0usize;
            for &c in value {
                if !c.is_ascii_digit() {
                    return Err(());
                }
                v = v * 10 + (c - b'0') as usize;
                if v > MAX_BODY {
                    return Err(());
                }
            }
            content_length = v;
            have_length = true;
        } else if ci_eq(name, b"connection") {
            if value.len() == 5 && ci_eq(value, b"close") {
                keep_alive = false;
            }
        } else if ci_eq(name, b"expect") {
            if ci_eq(value, b"100-continue") {
                expect_continue = true;
            }
        } else if ci_eq(name, b"transfer-encoding") {
            return Err(());
        }
    }
    if !have_length {
        content_length = 0;
    }
    if head_end + content_length > buf.len() {
        return if head_end + content_length > MAX_HEAD + MAX_BODY {
            Err(())
        } else {
            Ok(None)
        };
    }

    Ok(Some((
        Request {
            method,
            path,
            auth,
            body: &buf[head_end..head_end + content_length],
            keep_alive,
            expect_continue,
        },
        head_end + content_length,
    )))
}

/// Append a JSON string literal, escaping only what JSON requires.
pub fn put_jstr(out: &mut Vec<u8>, s: &[u8]) {
    out.push(b'"');
    let mut run = 0usize;
    for (i, &c) in s.iter().enumerate() {
        if c != b'"' && c != b'\\' && c >= 0x20 {
            continue;
        }
        if i > run {
            out.extend_from_slice(&s[run..i]);
        }
        match c {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            _ => {
                const HEX: &[u8; 16] = b"0123456789abcdef";
                out.extend_from_slice(b"\\u00");
                out.push(HEX[(c >> 4) as usize]);
                out.push(HEX[(c & 0xf) as usize]);
            }
        }
        run = i + 1;
    }
    if s.len() > run {
        out.extend_from_slice(&s[run..]);
    }
    out.push(b'"');
}

pub fn put_i64(out: &mut Vec<u8>, v: i64) {
    let mut buf = [0u8; 24];
    let mut n = 0usize;
    let neg = v < 0;
    let mut u = if neg { (v as i128).unsigned_abs() as u128 } else { v as u128 };
    loop {
        buf[n] = b'0' + (u % 10) as u8;
        n += 1;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    if neg {
        out.push(b'-');
    }
    while n > 0 {
        n -= 1;
        out.push(buf[n]);
    }
}

fn reason(code: u16) -> &'static str {
    match code {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "Error",
    }
}

/// Append a complete response (status line, headers, body).
pub fn respond(out: &mut Vec<u8>, code: u16, body: &[u8]) {
    out.extend_from_slice(b"HTTP/1.1 ");
    put_i64(out, code as i64);
    out.push(b' ');
    out.extend_from_slice(reason(code).as_bytes());
    out.extend_from_slice(b"\r\nContent-Type: application/json\r\nContent-Length: ");
    put_i64(out, body.len() as i64);
    out.extend_from_slice(b"\r\n\r\n");
    out.extend_from_slice(body);
}
