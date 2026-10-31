//! Strict JSON validation with top-level member extraction.
//!
//! Used for request bodies (`{"body": "..."}`) and for JWT header/claims. The
//! whole document is validated, so malformed input is rejected even when the
//! field we want happens to parse.

pub const ABSENT: u8 = 0;
pub const STRING: u8 = 1;
pub const NUMBER: u8 = 2;
pub const TRUE: u8 = 3;
pub const FALSE: u8 = 4;
pub const NULL: u8 = 5;
pub const OBJECT: u8 = 6;
pub const ARRAY: u8 = 7;

const MAX_DEPTH: u32 = 64;

#[derive(Clone, Copy)]
pub struct Value<'a> {
    /// For strings: the raw contents between the quotes, escapes untouched.
    pub raw: &'a [u8],
    pub ty: u8,
}

impl<'a> Value<'a> {
    fn absent() -> Self {
        Value { raw: &[], ty: ABSENT }
    }
}

struct Parser<'a, 'k> {
    s: &'a [u8],
    i: usize,
    depth: u32,
    keys: &'k [&'k str],
    out: &'k mut [Value<'a>],
}

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r')
}

fn is_digit(c: u8) -> bool {
    c.is_ascii_digit()
}

fn hex(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

impl<'a, 'k> Parser<'a, 'k> {
    fn skip_space(&mut self) {
        while self.i < self.s.len() && is_space(self.s[self.i]) {
            self.i += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn string(&mut self) -> Result<&'a [u8], ()> {
        if self.peek() != Some(b'"') {
            return Err(());
        }
        self.i += 1;
        let start = self.i;
        loop {
            let c = self.peek().ok_or(())?;
            if c == b'"' {
                let end = self.i;
                self.i += 1;
                return Ok(&self.s[start..end]);
            }
            if c == b'\\' {
                self.i += 1;
                match self.peek().ok_or(())? {
                    b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't' => self.i += 1,
                    b'u' => {
                        self.i += 1;
                        for _ in 0..4 {
                            hex(self.peek().ok_or(())?).ok_or(())?;
                            self.i += 1;
                        }
                    }
                    _ => return Err(()),
                }
                continue;
            }
            if c < 0x20 {
                return Err(());
            }
            self.i += 1;
        }
    }

    fn number(&mut self) -> Result<&'a [u8], ()> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek().ok_or(())? {
            b'0' => self.i += 1,
            c if is_digit(c) => {
                while self.peek().map_or(false, is_digit) {
                    self.i += 1;
                }
            }
            _ => return Err(()),
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            if !self.peek().map_or(false, is_digit) {
                return Err(());
            }
            while self.peek().map_or(false, is_digit) {
                self.i += 1;
            }
        }
        if matches!(self.peek(), Some(b'e') | Some(b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+') | Some(b'-')) {
                self.i += 1;
            }
            if !self.peek().map_or(false, is_digit) {
                return Err(());
            }
            while self.peek().map_or(false, is_digit) {
                self.i += 1;
            }
        }
        Ok(&self.s[start..self.i])
    }

    fn literal(&mut self, lit: &'static [u8], ty: u8) -> Result<Value<'a>, ()> {
        if self.s.len() - self.i < lit.len() || &self.s[self.i..self.i + lit.len()] != lit {
            return Err(());
        }
        self.i += lit.len();
        Ok(Value { raw: lit, ty })
    }

    fn slot(&self, key: &[u8]) -> Option<usize> {
        self.keys.iter().position(|k| k.as_bytes() == key)
    }

    fn value(&mut self) -> Result<Value<'a>, ()> {
        if self.depth > MAX_DEPTH {
            return Err(());
        }
        self.skip_space();
        match self.peek().ok_or(())? {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => Ok(Value { raw: self.string()?, ty: STRING }),
            b't' => self.literal(b"true", TRUE),
            b'f' => self.literal(b"false", FALSE),
            b'n' => self.literal(b"null", NULL),
            _ => Ok(Value { raw: self.number()?, ty: NUMBER }),
        }
    }

    fn object(&mut self) -> Result<Value<'a>, ()> {
        let top = self.depth == 0;
        self.i += 1;
        self.skip_space();
        if self.peek() == Some(b'}') {
            self.i += 1;
            return Ok(Value { raw: &[], ty: OBJECT });
        }
        loop {
            let key = self.string()?;
            self.skip_space();
            if self.peek() != Some(b':') {
                return Err(());
            }
            self.i += 1;
            let slot = if top { self.slot(key) } else { None };
            self.depth += 1;
            let v = self.value();
            self.depth -= 1;
            let v = v?;
            if let Some(k) = slot {
                self.out[k] = v;
            }
            self.skip_space();
            match self.peek().ok_or(())? {
                b',' => {
                    self.i += 1;
                    self.skip_space();
                }
                b'}' => {
                    self.i += 1;
                    return Ok(Value { raw: &[], ty: OBJECT });
                }
                _ => return Err(()),
            }
        }
    }

    fn array(&mut self) -> Result<Value<'a>, ()> {
        self.i += 1;
        self.skip_space();
        if self.peek() == Some(b']') {
            self.i += 1;
            return Ok(Value { raw: &[], ty: ARRAY });
        }
        loop {
            self.depth += 1;
            let v = self.value();
            self.depth -= 1;
            v?;
            self.skip_space();
            match self.peek().ok_or(())? {
                b',' => {
                    self.i += 1;
                    self.skip_space();
                }
                b']' => {
                    self.i += 1;
                    return Ok(Value { raw: &[], ty: ARRAY });
                }
                _ => return Err(()),
            }
        }
    }
}

/// Validate `doc` and fill `out[k]` with the top-level member named `keys[k]`.
pub fn scan<'a, 'k>(
    doc: &'a [u8],
    keys: &'k [&'k str],
    out: &'k mut [Value<'a>],
) -> Result<(), ()> {
    assert_eq!(keys.len(), out.len());
    for slot in out.iter_mut() {
        *slot = Value::absent();
    }
    let mut p = Parser { s: doc, i: 0, depth: 0, keys, out };
    p.value()?;
    p.skip_space();
    if p.i != p.s.len() {
        return Err(());
    }
    Ok(())
}

/// Decode a JSON string body (already validated) into UTF-8 bytes.
pub fn unescape(raw: &[u8], out: &mut Vec<u8>) -> Result<(), ()> {
    let mut i = 0;
    while i < raw.len() {
        let c = raw[i];
        i += 1;
        if c != b'\\' {
            out.push(c);
            continue;
        }
        let e = *raw.get(i).ok_or(())?;
        i += 1;
        let cp: u32 = match e {
            b'"' => 0x22,
            b'\\' => 0x5c,
            b'/' => 0x2f,
            b'b' => 0x08,
            b'f' => 0x0c,
            b'n' => 0x0a,
            b'r' => 0x0d,
            b't' => 0x09,
            b'u' => {
                let mut v = 0u32;
                for _ in 0..4 {
                    v = (v << 4) | hex(*raw.get(i).ok_or(())?).ok_or(())? as u32;
                    i += 1;
                }
                if (0xd800..=0xdbff).contains(&v) {
                    if raw.get(i) != Some(&b'\\') || raw.get(i + 1) != Some(&b'u') {
                        return Err(());
                    }
                    i += 2;
                    let mut lo = 0u32;
                    for _ in 0..4 {
                        lo = (lo << 4) | hex(*raw.get(i).ok_or(())?).ok_or(())? as u32;
                        i += 1;
                    }
                    if !(0xdc00..=0xdfff).contains(&lo) {
                        return Err(());
                    }
                    0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00)
                } else if (0xdc00..=0xdfff).contains(&v) {
                    return Err(());
                } else {
                    v
                }
            }
            _ => return Err(()),
        };
        if cp < 0x80 {
            out.push(cp as u8);
        } else if cp < 0x800 {
            out.push(0xc0 | (cp >> 6) as u8);
            out.push(0x80 | (cp & 0x3f) as u8);
        } else if cp < 0x10000 {
            out.push(0xe0 | (cp >> 12) as u8);
            out.push(0x80 | ((cp >> 6) & 0x3f) as u8);
            out.push(0x80 | (cp & 0x3f) as u8);
        } else {
            out.push(0xf0 | (cp >> 18) as u8);
            out.push(0x80 | ((cp >> 12) & 0x3f) as u8);
            out.push(0x80 | ((cp >> 6) & 0x3f) as u8);
            out.push(0x80 | (cp & 0x3f) as u8);
        }
    }
    Ok(())
}

/// Parse a JSON integer token. Returns `None` for fractions, exponents, overflow.
pub fn as_int(v: &Value) -> Option<i64> {
    if v.ty != NUMBER {
        return None;
    }
    let mut neg = false;
    let mut i = 0;
    if v.raw.first() == Some(&b'-') {
        neg = true;
        i = 1;
    }
    if i >= v.raw.len() {
        return None;
    }
    let mut acc: i64 = 0;
    for &c in &v.raw[i..] {
        if !c.is_ascii_digit() {
            return None;
        }
        acc = acc.checked_mul(10)?.checked_add((c - b'0') as i64)?;
    }
    Some(if neg { -acc } else { acc })
}
