//! Compact JSON output written directly into a byte buffer.
//!
//! Response objects are formatted by hand so each response is reserved once,
//! no intermediate `serde_json::Value` tree is built, and SQLite row text can
//! be escaped straight from its borrowed bytes.

use itoa::Buffer;

pub struct Writer {
    buf: Vec<u8>,
}

impl Writer {
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            buf: Vec::with_capacity(capacity),
        }
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.buf
    }

    #[inline]
    pub fn raw(&mut self, text: &str) {
        self.buf.extend_from_slice(text.as_bytes());
    }

    #[inline]
    pub fn byte(&mut self, byte: u8) {
        self.buf.push(byte);
    }

    #[inline]
    pub fn int(&mut self, value: i64) {
        let mut buffer = Buffer::new();
        self.buf.extend_from_slice(buffer.format(value).as_bytes());
    }

    #[inline]
    pub fn bool(&mut self, value: bool) {
        self.raw(if value { "true" } else { "false" });
    }

    /// Writes a JSON string, escaping quotes, backslashes and control bytes.
    /// Valid UTF-8 is passed through unchanged (JSON permits raw UTF-8).
    pub fn string(&mut self, text: &str) {
        self.string_bytes(text.as_bytes());
    }

    pub fn string_bytes(&mut self, bytes: &[u8]) {
        const HEX: &[u8; 16] = b"0123456789abcdef";

        self.byte(b'"');
        let mut start = 0;
        for (index, &byte) in bytes.iter().enumerate() {
            let escape: &[u8] = match byte {
                b'"' => b"\\\"",
                b'\\' => b"\\\\",
                b'\n' => b"\\n",
                b'\r' => b"\\r",
                b'\t' => b"\\t",
                0x08 => b"\\b",
                0x0c => b"\\f",
                0x00..=0x1f => {
                    self.buf.extend_from_slice(&bytes[start..index]);
                    self.buf.extend_from_slice(b"\\u00");
                    self.buf.push(HEX[(byte >> 4) as usize]);
                    self.buf.push(HEX[(byte & 0x0f) as usize]);
                    start = index + 1;
                    continue;
                }
                _ => continue,
            };
            self.buf.extend_from_slice(&bytes[start..index]);
            self.buf.extend_from_slice(escape);
            start = index + 1;
        }
        self.buf.extend_from_slice(&bytes[start..]);
        self.byte(b'"');
    }
}

/// `{"id":…,"body":…,"created_at":…,"author":…,"like_count":…}`
pub fn post(
    out: &mut Writer,
    id: i64,
    body: &[u8],
    created_at: &[u8],
    author: &[u8],
    like_count: i64,
) {
    out.raw("{\"id\":");
    out.int(id);
    out.raw(",\"body\":");
    out.string_bytes(body);
    out.raw(",\"created_at\":");
    out.string_bytes(created_at);
    out.raw(",\"author\":");
    out.string_bytes(author);
    out.raw(",\"like_count\":");
    out.int(like_count);
    out.byte(b'}');
}

pub fn error(message: &str) -> Vec<u8> {
    let mut out = Writer::with_capacity(message.len() + 16);
    out.raw("{\"error\":");
    out.string(message);
    out.byte(b'}');
    out.into_bytes()
}

pub fn health_ok(uptime_s: u64) -> Vec<u8> {
    let mut out = Writer::with_capacity(64);
    out.raw("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    out.int(uptime_s as i64);
    out.byte(b'}');
    out.into_bytes()
}

pub fn health_degraded(error: &str) -> Vec<u8> {
    let mut out = Writer::with_capacity(error.len() + 64);
    out.raw("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
    out.string(error);
    out.byte(b'}');
    out.into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rendered(write: impl FnOnce(&mut Writer)) -> String {
        let mut out = Writer::with_capacity(64);
        write(&mut out);
        String::from_utf8(out.into_bytes()).unwrap()
    }

    #[test]
    fn escapes_strings() {
        assert_eq!(
            rendered(|out| out.string("a\"b\\c\n\t\u{1}")),
            r#""a\"b\\c\n\t\u0001""#
        );
        assert_eq!(rendered(|out| out.string("café ✓")), "\"café ✓\"");
    }

    #[test]
    fn formats_post_in_spec_order() {
        assert_eq!(
            rendered(|out| post(out, 7, b"hi", b"2025-12-31T23:59:19.409Z", b"user", 3)),
            r#"{"id":7,"body":"hi","created_at":"2025-12-31T23:59:19.409Z","author":"user","like_count":3}"#
        );
    }

    #[test]
    fn formats_errors_and_health() {
        assert_eq!(
            String::from_utf8(error("not found")).unwrap(),
            r#"{"error":"not found"}"#
        );
        assert_eq!(
            String::from_utf8(health_ok(12)).unwrap(),
            r#"{"status":"ok","db":"ok","uptime_s":12}"#
        );
        assert_eq!(
            String::from_utf8(health_degraded("disk \"full\"")).unwrap(),
            r#"{"status":"degraded","db":"unreachable","error":"disk \"full\""}"#
        );
    }
}
