//! One capacity check per post, including the worst-case JSON expansion. The
//! writer is private: every write is bounded by that reservation, and Vec's
//! length is published only after the whole post has been initialized.
struct Writer(*mut u8);
impl Writer {
    #[inline(always)]
    unsafe fn bytes(&mut self, input: &[u8]) {
        unsafe {
            std::ptr::copy_nonoverlapping(input.as_ptr(), self.0, input.len());
            self.0 = self.0.add(input.len());
        }
    }
    #[inline(always)]
    unsafe fn byte(&mut self, byte: u8) {
        unsafe {
            self.0.write(byte);
            self.0 = self.0.add(1);
        }
    }
    #[inline]
    unsafe fn string(&mut self, input: &[u8]) {
        unsafe {
            self.byte(b'"');
            let mut at = 0;
            let mut start = 0;
            while at < input.len() {
                if input.len() - at >= 16 && crate::plain16(&input[at..]) {
                    at += 16;
                    continue;
                }
                // An overlapping final vector covers the tail without an
                // out-of-bounds read or a scalar branch for every plain byte.
                if input.len() - at < 16 && input.len() >= 16
                    && crate::plain16(&input[input.len() - 16..]) {
                    break;
                }
                let c = input[at];
                if c < 32 || c == b'"' || c == b'\\' {
                    self.bytes(&input[start..at]);
                    match c {
                        b'"' => self.bytes(b"\\\""),
                        b'\\' => self.bytes(b"\\\\"),
                        b'\n' => self.bytes(b"\\n"),
                        b'\r' => self.bytes(b"\\r"),
                        b'\t' => self.bytes(b"\\t"),
                        _ => {
                            const HEX: &[u8] = b"0123456789abcdef";
                            self.bytes(&[
                                b'\\',
                                b'u',
                                b'0',
                                b'0',
                                HEX[(c >> 4) as usize],
                                HEX[(c & 15) as usize],
                            ]);
                        }
                    }
                    at += 1;
                    start = at;
                } else {
                    at += 1;
                }
            }
            self.bytes(&input[start..]);
            self.byte(b'"');
        }
    }
    #[inline]
    unsafe fn integer(&mut self, value: u64) {
        unsafe {
            self.bytes(itoa::Buffer::new().format(value).as_bytes());
        }
    }
}

pub fn post(id: i64, body: &[u8], created: &[u8], author: &[u8], likes: u64, out: &mut Vec<u8>) {
    let strings = body
        .len()
        .checked_add(created.len())
        .and_then(|n| n.checked_add(author.len()))
        .expect("JSON size overflow");
    let bound = strings
        .checked_mul(6)
        .and_then(|n| n.checked_add(128))
        .expect("JSON size overflow");
    out.reserve(bound);
    unsafe {
        let mut w = Writer(out.as_mut_ptr().add(out.len()));
        w.bytes(b"{\"id\":");
        w.integer(id as u64);
        w.bytes(b",\"body\":");
        w.string(body);
        w.bytes(b",\"created_at\":");
        w.string(created);
        w.bytes(b",\"author\":");
        w.string(author);
        w.bytes(b",\"like_count\":");
        w.integer(likes);
        w.byte(b'}');
        let end = w.0.offset_from(out.as_ptr()) as usize;
        debug_assert!(end - out.len() <= bound);
        out.set_len(end);
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn escaping_and_capacity_match_independent_json_serializer() {
        let controls: String = (0u8..32).map(char::from).collect();
        for pattern in ["a", "\\\"\n\r\t", "😀名", "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &controls] {
            for n in [0, 1, 15, 16, 17, 31, 32, 500] {
                let body = pattern.repeat(n);
                let mut out = b"prefix".to_vec();
                super::post(
                    i64::MAX,
                    body.as_bytes(),
                    b"timestamp",
                    body.as_bytes(),
                    u64::MAX,
                    &mut out,
                );
                let got: serde_json::Value = serde_json::from_slice(&out[6..]).unwrap();
                assert_eq!(
                    got,
                    serde_json::json!({"id": i64::MAX, "body": body,
                    "created_at": "timestamp", "author": body, "like_count": u64::MAX})
                );
            }
        }
    }
}
