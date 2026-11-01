//! A validating JSON reader for request bodies and JWT parts, and the string escaper for output.
//! It checks the whole document and reports the top-level object's string and number members;
//! everything else is validated and skipped without allocating.

pub enum Val<'a> {
    /// A string's raw bytes between the quotes, and whether it contains escapes.
    Str(&'a [u8], bool),
    Num(f64),
    Other,
}

const MAX_DEPTH: u32 = 64;

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

impl<'a> P<'a> {
    #[inline]
    fn ws(&mut self) {
        while let Some(&c) = self.s.get(self.i) {
            if c == b' ' || c == b'\n' || c == b'\r' || c == b'\t' {
                self.i += 1;
            } else {
                break;
            }
        }
    }
    #[inline]
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }
    fn lit(&mut self, w: &[u8]) -> Result<(), ()> {
        if self.s[self.i..].starts_with(w) {
            self.i += w.len();
            Ok(())
        } else {
            Err(())
        }
    }
    /// After the opening quote. Returns the raw contents and whether they hold escapes.
    fn string(&mut self) -> Result<(&'a [u8], bool), ()> {
        let start = self.i;
        let mut esc = false;
        loop {
            let c = *self.s.get(self.i).ok_or(())?;
            match c {
                b'"' => {
                    let raw = &self.s[start..self.i];
                    self.i += 1;
                    return Ok((raw, esc));
                }
                b'\\' => {
                    esc = true;
                    let e = *self.s.get(self.i + 1).ok_or(())?;
                    match e {
                        b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't' => self.i += 2,
                        b'u' => {
                            let h = self.s.get(self.i + 2..self.i + 6).ok_or(())?;
                            if !h.iter().all(u8::is_ascii_hexdigit) {
                                return Err(());
                            }
                            self.i += 6;
                        }
                        _ => return Err(()),
                    }
                }
                0..=0x1f => return Err(()),
                _ => self.i += 1,
            }
        }
    }
    fn number(&mut self) -> Result<f64, ()> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek() {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.i += 1;
                }
            }
            _ => return Err(()),
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(());
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(());
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        // The slice is ASCII digits and signs, so this is valid UTF-8.
        std::str::from_utf8(&self.s[start..self.i]).map_err(|_| ())?.parse().map_err(|_| ())
    }
    fn value(&mut self, depth: u32) -> Result<Val<'a>, ()> {
        if depth > MAX_DEPTH {
            return Err(());
        }
        self.ws();
        match self.peek().ok_or(())? {
            b'"' => {
                self.i += 1;
                let (s, e) = self.string()?;
                Ok(Val::Str(s, e))
            }
            b'{' => {
                self.object(depth, &mut |_, _| {})?;
                Ok(Val::Other)
            }
            b'[' => {
                self.i += 1;
                self.ws();
                if self.peek() == Some(b']') {
                    self.i += 1;
                    return Ok(Val::Other);
                }
                loop {
                    self.value(depth + 1)?;
                    self.ws();
                    match self.peek() {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            return Ok(Val::Other);
                        }
                        _ => return Err(()),
                    }
                }
            }
            b't' => self.lit(b"true").map(|_| Val::Other),
            b'f' => self.lit(b"false").map(|_| Val::Other),
            b'n' => self.lit(b"null").map(|_| Val::Other),
            _ => self.number().map(Val::Num),
        }
    }
    fn object(&mut self, depth: u32, f: &mut dyn FnMut(&[u8], Val<'a>)) -> Result<(), ()> {
        self.i += 1; // '{'
        self.ws();
        if self.peek() == Some(b'}') {
            self.i += 1;
            return Ok(());
        }
        loop {
            self.ws();
            if self.peek() != Some(b'"') {
                return Err(());
            }
            self.i += 1;
            let (k, kesc) = self.string()?;
            self.ws();
            if self.peek() != Some(b':') {
                return Err(());
            }
            self.i += 1;
            let v = self.value(depth + 1)?;
            if kesc {
                let mut name = Vec::new();
                unescape(k, &mut name);
                f(&name, v);
            } else {
                f(k, v);
            }
            self.ws();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b'}') => {
                    self.i += 1;
                    return Ok(());
                }
                _ => return Err(()),
            }
        }
    }
}

/// Validates `s` as one JSON document. If it is an object, `f` sees each top-level member
/// (later duplicates after earlier ones, so the last one wins, as in JSON.parse).
/// Returns Err for malformed JSON, Ok(false) for valid JSON that isn't an object.
pub fn parse_object<'a>(s: &'a [u8], f: &mut dyn FnMut(&[u8], Val<'a>)) -> Result<bool, ()> {
    if std::str::from_utf8(s).is_err() {
        return Err(());
    }
    let mut p = P { s, i: 0 };
    p.ws();
    let obj = p.peek() == Some(b'{');
    if obj {
        p.object(0, f)?;
    } else {
        p.value(0)?;
    }
    p.ws();
    if p.i != s.len() {
        return Err(());
    }
    Ok(obj)
}

fn hex4(h: &[u8]) -> u32 {
    h.iter().fold(0, |a, &c| a << 4 | (c as char).to_digit(16).unwrap_or(0))
}

/// Decodes a validated string's raw contents into `out`. Lone surrogates become U+FFFD.
pub fn unescape(raw: &[u8], out: &mut Vec<u8>) {
    let mut i = 0;
    while i < raw.len() {
        let c = raw[i];
        if c != b'\\' {
            let j = raw[i..].iter().position(|&b| b == b'\\').map_or(raw.len(), |p| i + p);
            out.extend_from_slice(&raw[i..j]);
            i = j;
            continue;
        }
        let e = raw[i + 1];
        i += 2;
        let ch = match e {
            b'b' => 8,
            b'f' => 12,
            b'n' => b'\n' as u32,
            b'r' => b'\r' as u32,
            b't' => b'\t' as u32,
            b'u' => {
                let mut u = hex4(&raw[i..i + 4]);
                i += 4;
                if (0xd800..0xdc00).contains(&u)
                    && raw.get(i..i + 2) == Some(b"\\u")
                    && raw.len() >= i + 6
                {
                    let lo = hex4(&raw[i + 2..i + 6]);
                    if (0xdc00..0xe000).contains(&lo) {
                        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                        i += 6;
                    }
                }
                u
            }
            other => other as u32,
        };
        let ch = char::from_u32(ch).unwrap_or('\u{fffd}');
        let mut b = [0u8; 4];
        out.extend_from_slice(ch.encode_utf8(&mut b).as_bytes());
    }
}

const fn esc_table() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut i = 0;
    while i < 0x20 {
        t[i] = 1;
        i += 1;
    }
    t[b'"' as usize] = 1;
    t[b'\\' as usize] = 1;
    t
}
static ESC: [u8; 256] = esc_table();

/// Nonzero if any byte of `w` is < 0x20, '"' or '\\' (exact as a yes/no answer).
#[inline]
fn word_needs_escape(w: u64) -> u64 {
    const ONES: u64 = 0x0101_0101_0101_0101;
    const HIGH: u64 = 0x8080_8080_8080_8080;
    let ctl = w.wrapping_sub(ONES * 0x20) & !w;
    let q = w ^ (ONES * b'"' as u64);
    let q = q.wrapping_sub(ONES) & !q;
    let b = w ^ (ONES * b'\\' as u64);
    let b = b.wrapping_sub(ONES) & !b;
    (ctl | q | b) & HIGH
}

/// Appends `s` as a JSON string body (no quotes). Non-ASCII passes through as UTF-8.
#[inline]
pub fn escape_into(out: &mut Vec<u8>, s: &[u8]) {
    // Most strings need no escaping: check 8 bytes at a time and copy them in one go.
    let mut i = 0;
    while i + 8 <= s.len() {
        let w = u64::from_le_bytes(s[i..i + 8].try_into().unwrap());
        if word_needs_escape(w) != 0 {
            break;
        }
        i += 8;
    }
    if i == s.len() || !s[i..].iter().any(|&c| ESC[c as usize] != 0) {
        out.extend_from_slice(s);
        return;
    }
    escape_slow(out, s);
}

#[cold]
fn escape_slow(out: &mut Vec<u8>, s: &[u8]) {
    let mut start = 0;
    for (i, &c) in s.iter().enumerate() {
        if ESC[c as usize] == 0 {
            continue;
        }
        out.extend_from_slice(&s[start..i]);
        match c {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            8 => out.extend_from_slice(b"\\b"),
            12 => out.extend_from_slice(b"\\f"),
            _ => {
                const H: &[u8; 16] = b"0123456789abcdef";
                out.extend_from_slice(&[b'\\', b'u', b'0', b'0', H[(c >> 4) as usize], H[(c & 15) as usize]]);
            }
        }
        start = i + 1;
    }
    out.extend_from_slice(&s[start..]);
}

/// "00".."99", for writing two digits per division.
const DIGITS2: &[u8; 200] = b"00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

#[inline]
pub fn push_int(out: &mut Vec<u8>, v: i64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    let mut n = v.unsigned_abs();
    while n >= 100 {
        let r = (n % 100) as usize * 2;
        n /= 100;
        i -= 2;
        buf[i..i + 2].copy_from_slice(&DIGITS2[r..r + 2]);
    }
    if n >= 10 {
        let r = n as usize * 2;
        i -= 2;
        buf[i..i + 2].copy_from_slice(&DIGITS2[r..r + 2]);
    } else {
        i -= 1;
        buf[i] = b'0' + n as u8;
    }
    if v < 0 {
        out.push(b'-');
    }
    out.extend_from_slice(&buf[i..]);
}
