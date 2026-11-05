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
    const M01: u64 = 0x0101_0101_0101_0101;
    const M80: u64 = 0x8080_8080_8080_8080;
    const CR: u64 = 0x0d0d_0d0d_0d0d_0d0d;
    const LF: u64 = 0x0a0a_0a0a_0a0a_0a0a;
    // haszero via the SWAR idiom: some byte is 0x00. With `x ^ CR` it reads
    // "the chunk carries a carriage return"; with `x ^ LF`, a line feed.
    let cords = |x: u64| ((x.wrapping_sub(M01)) & !x & M80) != 0;
    let any_eol = |x: u64| cords(x ^ CR) || cords(x ^ LF);

    let mut head_end = 0usize;
    // The terminator `\\r\\n\\r\\n` contains only CR and LF, so a word that
    // carries neither can never complete it: skip eight bytes at a time and
    // fall to the byte check only on a word that has one.
    let mut i = 0usize;
    while i + 3 < buf.len() {
        if i + 8 <= buf.len() && !any_eol(u64::from_ne_bytes(buf[i..i + 8].try_into().unwrap())) {
            i += 8;
            continue;
        }
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
        // Line ends ride on CR or LF only; the same word-scan finds them. The
        // byte bound stays the scan's end so overflow past the terminator is
        // never read.
        while i + 8 <= buf.len() && i + 8 <= head_end
            && !any_eol(u64::from_ne_bytes(buf[i..i + 8].try_into().unwrap()))
        {
            i += 8;
        }
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

/// Append a JSON string literal, escaping only what JSON requires. The
/// common case — a run of bytes none of which can need escaping — is checked
/// eight bytes at a time, then copied in one extend; a detected unsafe byte
/// falls through to the per-byte loop for that position.
pub fn put_jstr(out: &mut Vec<u8>, s: &[u8]) {
    out.push(b'"');
    const M01: u64 = 0x0101_0101_0101_0101;
    const M80: u64 = 0x8080_8080_8080_8080;
    const Q: u64 = 0x2222_2222_2222_2222;
    const BS: u64 = 0x5c5c_5c5c_5c5c_5c5c;
    // Three reads of "some byte is unsafe": bytes below 0x20 (hasless via the
    // M01-scaled subtrahend... inlined, since `cords` is haszero), and bytes
    // equal to quote or backslash (haszero on the XOR).
    let safe_chunk = |x: u64| {
        let no_low = (x.wrapping_sub(0x2020_2020_2020_2020)) & !x & M80 == 0;
        let t = x ^ Q;
        let no_q = (t.wrapping_sub(M01)) & !t & M80 == 0;
        let t = x ^ BS;
        let no_bs = (t.wrapping_sub(M01)) & !t & M80 == 0;
        no_low && no_q && no_bs
    };
    let mut i = 0usize;
    let h = s.len() & !7;
    while i < h && safe_chunk(u64::from_ne_bytes(s[i..i + 8].try_into().unwrap())) {
        i += 8;
    }
    if i > 0 {
        out.extend_from_slice(&s[..i]);
    }
    let mut run = i;
    for (j, &c) in s[i..].iter().enumerate() {
        if c != b'"' && c != b'\\' && c >= 0x20 {
            continue;
        }
        let k = i + j;
        if k > run {
            out.extend_from_slice(&s[run..k]);
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
        run = k + 1;
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
    // One reservation for the whole response: a fresh buffer would otherwise
    // grow geometrically, copying a multi-KB feed body about a dozen times.
    out.reserve(128 + body.len());
    out.extend_from_slice(b"HTTP/1.1 ");
    put_i64(out, code as i64);
    out.push(b' ');
    out.extend_from_slice(reason(code).as_bytes());
    out.extend_from_slice(b"\r\nContent-Type: application/json\r\nContent-Length: ");
    put_i64(out, body.len() as i64);
    out.extend_from_slice(b"\r\n\r\n");
    out.extend_from_slice(body);
}

/// The eight-byte scan in `put_jstr` must produce byte-for-byte what the
/// per-byte loop produces; these cases feed literal quotes, backslashes,
/// control bytes, DEL, and UTF-8 text through both orderings.
#[cfg(test)]
mod jstr_tests {
    use super::put_jstr;

    /// Scalar-only reference (no word scan), for comparison.
    fn put_jstr_scalar(out: &mut Vec<u8>, s: &[u8]) {
        out.push(b'"');
        for &c in s {
            if c == b'"' || c == b'\\' || c < 0x20 {
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
            } else {
                out.push(c);
            }
        }
        out.push(b'"');
    }

    fn round(tokens: &[u8]) -> bool {
        let mut a = Vec::new();
        let mut b = Vec::new();
        put_jstr(&mut a, tokens);
        put_jstr_scalar(&mut b, tokens);
        a == b
    }

    #[test]
    fn agrees_with_the_scalar_reference() {
        let plain = b"a plain body with no escapable byte at all, longer than eight";
        assert!(round(plain));
        // every alignment offset around the unsafe byte
        for pos in 0..16 {
            let mut s = vec![b'x'; 24 + pos];
            s.insert(8 + pos, b'"');
            assert!(round(&s), "quote at +{}", pos);
        }
        for pos in 0..16 {
            let mut s = vec![b'y'; 24 + pos];
            s.insert(7 + pos, b'\\');
            assert!(round(&s), "backslash at +{}", pos);
        }
        for c in [
            0x00u8, 0x01, 0x08, 0x09, 0x0a, 0x0d, 0x1f, 0x20, 0x22, 0x23, 0x5b, 0x5c, 0x5d,
            0x7e, 0x7f, 0x80, 0xff,
        ] {
            let mut s = vec![b'z'; 17];
            s[3] = c;
            s[11] = c;
            assert!(round(&s), "byte {c:#04x}");
        }
        // quote and backslash inside the same word-sized window, then several
        // unsafe bytes in a row
        assert!(round(b"aaa\"bbbb\\cccc\n\r\tddd\"\\e\xff\0"));
        assert!(round(b"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\""));
        assert!(round(b"\\\\\\\\\\\\\\\\"));
    }
}

#[cfg(test)]
mod framing_tests {
    use super::parse;

    fn get(path: &[u8], head: &[u8]) -> (bool, usize) {
        let mut req_bytes = Vec::new();
        req_bytes.extend_from_slice(head);
        match parse(&req_bytes).unwrap() {
            Some((r, used)) => (r.path == path, used),
            None => (false, 0),
        }
    }

    #[test]
    fn finds_the_terminator_across_chunks_and_line_shapes() {
        // terminator lands at every alignment offset the word skip crosses
        for pad in 0..20 {
            let mut req_bytes = Vec::new();
            req_bytes.extend_from_slice(b"GET /x");
            req_bytes.extend_from_slice(&vec![b'x'; pad]);
            req_bytes.extend_from_slice(b" HTTP/1.1\r\nHost: x\r\n\r\n");
            match parse(&req_bytes).unwrap() {
                Some((r, used)) => {
                    assert!(r.path == b"/x" || pad > 0 && r.path.starts_with(b"/x"),
                        "pad {pad}");
                    assert!(used == req_bytes.len());
                }
                None => panic!("pad {pad}: not framed"),
            }
        }
        // bare-LF lines do not terminate a request head (same as the byte
        // scan this replaced): only `\\r\\n\\r\\n` frames.
        let b = b"GET /y HTTP/1.1\nHost: x\n\nZ\n\n";
        match parse(b).unwrap() {
            None => {}
            _ => panic!("bare LF must not frame"),
        }
        // tabs inside a header value must not confuse the terminator scan
        let b = b"GET /t HTTP/1.1\r\nX-A: \t\tval\r\nHost: x\r\n\r\n";
        let (path_ok, _) = get(b"/t", b);
        assert!(path_ok);
        // content-length still honoured after the fast scan
        let b = b"POST /p HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
        match parse(b).unwrap() {
            Some((r, used)) => {
                assert!(r.method == b"POST");
                assert!(used == b.len(), "used {used}");
                assert!(r.body == b"hello");
            }
            None => panic!("POST not framed"),
        }
        // no terminator yet
        match parse(b"GET / HTTP/1.1\r\nHost: ").unwrap() {
            None => {}
            _ => panic!("incomplete input must read more"),
        }
        // oversized header block is an error
        let big = vec![b'a'; super::MAX_HEAD + 1];
        assert!(parse(&big).is_err());
    }
}
