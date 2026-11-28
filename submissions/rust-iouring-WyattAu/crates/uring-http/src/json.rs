//! Fixed-shape JSON response templates, written straight into pre-sized
//! buffers. Key order is fixed by the SPEC; the size prepass returns the
//! exact byte length so the event loop can send from one buffer with no
//! reallocation (Lean target: prepass length == written length for all
//! inputs; both total).
//!
//! Escaping: `"`, `\`, and bytes < 0x20 are escaped (`\"`, `\\`, `\u00XX`).
//! SPEC makes `<>&/` and non-ASCII escaping optional — we pass them through
//! as raw UTF-8 bytes (bodies arrive as raw TEXT bytes from SQLite records;
//! they are emitted verbatim, which is byte-identical to what went in).

/// A post as raw borrowed bytes straight from the record decoder.
#[derive(Debug, Clone, Copy)]
pub struct PostRef<'a> {
    pub id: i64,
    pub body: &'a [u8],
    /// Exactly the stored format: `2025-12-31T23:59:19.409Z` (24 bytes).
    pub created_at: &'a [u8],
    pub author: &'a [u8],
    pub like_count: i64,
}

/// Escaped JSON-string length of `bytes` (quotes added by caller shape).
#[must_use]
pub fn escaped_len(bytes: &[u8]) -> usize {
    bytes
        .iter()
        .map(|&b| match b {
            b'"' | b'\\' => 2,
            b if b < 0x20 => 6, // \u00XX
            _ => 1,
        })
        .sum()
}

/// Write `bytes` as a JSON string body (no surrounding quotes) into `out`.
/// Returns bytes written; caller sized `out` via [`escaped_len`].
pub fn write_escaped(out: &mut [u8], bytes: &[u8]) -> usize {
    let mut n = 0;
    for &b in bytes {
        match b {
            b'"' => {
                out[n..n + 2].copy_from_slice(b"\\\"");
                n += 2;
            }
            b'\\' => {
                out[n..n + 2].copy_from_slice(b"\\\\");
                n += 2;
            }
            b if b < 0x20 => {
                out[n] = b'\\';
                out[n + 1] = b'u';
                out[n + 2] = b'0';
                out[n + 3] = b'0';
                out[n + 4] = HEX[(b >> 4) as usize];
                out[n + 5] = HEX[(b & 0xf) as usize];
                n += 6;
            }
            _ => {
                out[n] = b;
                n += 1;
            }
        }
    }
    n
}

const HEX: &[u8; 16] = b"0123456789abcdef";

#[inline]
fn decimal_len(mut v: i64) -> usize {
    if v == 0 {
        return 1;
    }
    let neg = usize::from(v < 0);
    let mut n = 0;
    while v != 0 {
        v /= 10;
        n += 1;
    }
    neg + n
}

/// Fixed structural bytes of the post object (between the five values):
/// `{"id":` + `,"body":"` + `","created_at":"` + `","author":"` +
/// `","like_count":` + `}` = 6 + 9 + 16 + 12 + 15 + 1 = 59.
const POST_STRUCTURE_LEN: usize = 59;

/// Exact length of the post object: {"id":…,"body":…,"created_at":…,"author":…,"like_count":…}
#[must_use]
pub fn post_json_len(p: &PostRef<'_>) -> usize {
    POST_STRUCTURE_LEN
        + decimal_len(p.id)
        + escaped_len(p.body)
        + p.created_at.len()
        + escaped_len(p.author)
        + decimal_len(p.like_count)
}

/// Write the post object into `out`. Returns bytes written.
/// Contract: equals `post_json_len(p)` when `out.len() >= post_json_len(p)`
/// (asserted in tests; prepass and writer must agree for all inputs).
pub fn write_post_json(out: &mut [u8], p: &PostRef<'_>) -> usize {
    let mut n = 0;
    macro_rules! raw {
        ($bytes:expr) => {{
            let b: &[u8] = $bytes;
            out[n..n + b.len()].copy_from_slice(b);
            n += b.len();
        }};
    }
    raw!(b"{\"id\":");
    n += write_decimal(out, n, p.id);
    raw!(b",\"body\":\"");
    n += write_escaped(&mut out[n..], p.body);
    raw!(b"\",\"created_at\":\"");
    out[n..n + p.created_at.len()].copy_from_slice(p.created_at);
    n += p.created_at.len();
    raw!(b"\",\"author\":\"");
    n += write_escaped(&mut out[n..], p.author);
    raw!(b"\",\"like_count\":");
    n += write_decimal(out, n, p.like_count);
    raw!(b"}");
    n
}

/// i64 decimal into `out[off..]`, returns bytes written. -0 emits "0".
pub fn write_decimal(out: &mut [u8], off: usize, v: i64) -> usize {
    if v == 0 {
        out[off] = b'0';
        return 1;
    }
    let neg = v < 0;
    // i64::MIN magnitude overflows i64; do unsigned magnitude via i128.
    let mut mag = (v as i128).unsigned_abs();
    let mut tmp = [0u8; 20];
    let mut i = tmp.len();
    while mag > 0 {
        i -= 1;
        tmp[i] = b'0' + (mag % 10) as u8;
        mag /= 10;
    }
    let mut n = 0;
    if neg {
        out[off] = b'-';
        n = 1;
    }
    out[off + n..off + n + tmp.len() - i].copy_from_slice(&tmp[i..]);
    n + tmp.len() - i
}

/// Feed wrapper: {"posts":[post,…]} — `{"posts":[` + values + commas + `]}`.
#[must_use]
pub fn feed_json_len(posts: &[PostRef<'_>]) -> usize {
    12 + posts.len().saturating_sub(1) + posts.iter().map(post_json_len).sum::<usize>()
}

/// Write the feed wrapper. Contract mirrors [`write_post_json`].
pub fn write_feed_json(out: &mut [u8], posts: &[PostRef<'_>]) -> usize {
    let mut n = 0;
    out[..10].copy_from_slice(b"{\"posts\":[");
    n += 10;
    for (i, p) in posts.iter().enumerate() {
        if i > 0 {
            out[n] = b',';
            n += 1;
        }
        n += write_post_json(&mut out[n..], p);
    }
    out[n] = b']';
    out[n + 1] = b'}';
    n + 2
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(p: &PostRef<'_>) -> (usize, String) {
        let len = post_json_len(p);
        let mut buf = vec![0u8; len];
        let n = write_post_json(&mut buf, p);
        (n, String::from_utf8(buf).unwrap())
    }

    #[test]
    fn simple_post_shape_and_order() {
        let p = PostRef {
            id: 42,
            body: b"hello",
            created_at: b"2026-11-21T15:43:41.828Z",
            author: b"wyatt",
            like_count: 7,
        };
        let (n, s) = roundtrip(&p);
        assert_eq!(n, post_json_len(&p));
        assert_eq!(
            s,
            r#"{"id":42,"body":"hello","created_at":"2026-11-21T15:43:41.828Z","author":"wyatt","like_count":7}"#
        );
    }

    #[test]
    fn escaping_quotes_backslashes_and_controls() {
        let p = PostRef {
            id: 1,
            body: b"a\"b\\c\x01d",
            created_at: b"2026-01-01T00:00:00.000Z",
            author: b"u",
            like_count: 0,
        };
        let (n, s) = roundtrip(&p);
        assert_eq!(n, post_json_len(&p));
        assert!(s.contains(r#"a\"b\\c\u0001d"#), "{s}");
    }

    #[test]
    fn non_ascii_and_angle_brackets_pass_through() {
        let body = "<b>café</b>"; // pass-through per SPEC
        let p = PostRef {
            id: 1,
            body: body.as_bytes(),
            created_at: b"2026-01-01T00:00:00.000Z",
            author: b"u",
            like_count: 0,
        };
        let (_, s) = roundtrip(&p);
        assert!(s.contains("<b>café</b>"), "{s}");
    }

    #[test]
    fn negative_ids_take_sign_into_len() {
        let p = PostRef {
            id: -12,
            body: b"",
            created_at: b"2026-01-01T00:00:00.000Z",
            author: b"",
            like_count: -1,
        };
        let (n, s) = roundtrip(&p);
        assert_eq!(n, post_json_len(&p));
        assert!(s.contains(r#""id":-12,"#) && s.contains(r#""like_count":-1}"#), "{s}");
    }

    #[test]
    fn i64_min_roundtrips() {
        let p = PostRef {
            id: i64::MIN,
            body: b"",
            created_at: b"2026-01-01T00:00:00.000Z",
            author: b"",
            like_count: i64::MIN,
        };
        let (n, s) = roundtrip(&p);
        assert_eq!(n, post_json_len(&p));
        assert!(s.contains(r#""id":-9223372036854775808,"#), "{s}");
    }

    #[test]
    fn feed_wrapper_length_matches_write() {
        let posts = [
            PostRef { id: 1, body: b"a", created_at: b"2026-01-01T00:00:00.000Z", author: b"u", like_count: 0 },
            PostRef { id: 2, body: b"b\"c", created_at: b"2026-01-01T00:00:00.001Z", author: b"v", like_count: 3 },
        ];
        let len = feed_json_len(&posts);
        let mut buf = vec![0u8; len];
        let n = write_feed_json(&mut buf, &posts);
        assert_eq!(n, len);
        let s = String::from_utf8(buf).unwrap();
        assert!(s.starts_with(r#"{"posts":[{"id":1"#) && s.ends_with(r#""like_count":3}]}"#), "{s}");
    }

    #[test]
    fn empty_feed() {
        let len = feed_json_len(&[]);
        let mut buf = vec![0u8; len];
        let n = write_feed_json(&mut buf, &[]);
        assert_eq!(n, len);
        assert_eq!(String::from_utf8(buf).unwrap(), r#"{"posts":[]}"#);
    }
}
