use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::fmt;
use std::num::NonZeroI64;
use std::ops::Range;

const DECODE_MAX: usize = 49_152;
const HEADER_MAX: usize = 65_536;
const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

pub(crate) struct Auth {
    key: Hmac<Sha256>,
}

pub(crate) struct Claims {
    header: Vec<u8>,
    payload: Vec<u8>,
    accepted: Option<Accepted>,
}

struct Accepted {
    user_id: NonZeroI64,
    username: Range<usize>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum AuthError {
    Missing,
    Invalid,
    Payload,
}

impl fmt::Display for AuthError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Missing => "missing bearer token",
            Self::Invalid => "invalid or expired token",
            Self::Payload => "invalid token payload",
        })
    }
}
impl std::error::Error for AuthError {}

impl Claims {
    pub(crate) fn new() -> Self {
        Self {
            header: Vec::with_capacity(DECODE_MAX),
            payload: Vec::with_capacity(DECODE_MAX),
            accepted: None,
        }
    }

    pub(crate) fn user_id(&self) -> Option<i64> {
        self.accepted
            .as_ref()
            .map(|accepted| accepted.user_id.get())
    }

    pub(crate) fn username(&self) -> Option<&str> {
        self.accepted.as_ref().map(|accepted| {
            std::str::from_utf8(
                self.payload
                    .get(accepted.username.clone())
                    .expect("verify set the username range inside payload"),
            )
            .expect("accepted Unicode username")
        })
    }
}

impl Auth {
    pub(crate) fn new(secret: &[u8]) -> Self {
        Self {
            key: Hmac::<Sha256>::new_from_slice(secret).expect("HMAC accepts every key length"),
        }
    }

    /// # Errors
    /// Returns Missing for another bearer prefix, Invalid for token/signature/time
    /// rejection, and Payload for invalid claims after authentication succeeds.
    pub(crate) fn verify(
        &self,
        header: Option<&[u8]>,
        now_s: i64,
        scratch: &mut Claims,
    ) -> Result<(), AuthError> {
        scratch.accepted = None;
        scratch.header.clear();
        scratch.payload.clear();
        let result = self.verify_inner(header, now_s, scratch);
        if result.is_err() {
            scratch.header.clear();
            scratch.payload.clear();
        }
        result
    }

    fn verify_inner(
        &self,
        header: Option<&[u8]>,
        now_s: i64,
        scratch: &mut Claims,
    ) -> Result<(), AuthError> {
        let header = header.ok_or(AuthError::Missing)?;
        let token = header.strip_prefix(b"Bearer ").ok_or(AuthError::Missing)?;
        if header.len() > HEADER_MAX {
            return Err(AuthError::Invalid);
        }
        let mut segments = token.split(|&b| b == b'.');
        let head = segments.next().ok_or(AuthError::Invalid)?;
        let payload = segments.next().ok_or(AuthError::Invalid)?;
        let signature = segments.next().ok_or(AuthError::Invalid)?;
        if head.is_empty()
            || payload.is_empty()
            || signature.len() != 43
            || segments.next().is_some()
        {
            return Err(AuthError::Invalid);
        }
        decoded_len(head.len())?;
        decoded_len(payload.len())?;
        let mut mac = self.key.clone();
        mac.update(
            token
                .get(..head.len() + 1 + payload.len())
                .expect("head and payload are the token's first two dot-separated segments"),
        );
        let digest = mac.finalize().into_bytes();
        let canonical = encode_signature(&digest);
        // Both texts are 43 bytes (signature.len() was checked above); every pair is compared.
        let mut difference = 0_u8;
        for (s, c) in signature.iter().zip(canonical.iter()) {
            difference |= std::hint::black_box(s ^ c);
        }
        if difference != 0 {
            return Err(AuthError::Invalid);
        }
        let fields = decode_document(head, &mut scratch.header, &mut scratch.payload, true)?;
        let Some(Token::String(alg)) = fields[0] else {
            return Err(AuthError::Invalid);
        };
        if !alg.hs256 {
            return Err(AuthError::Invalid);
        }
        scratch.payload.clear();
        let fields = decode_document(payload, &mut scratch.payload, &mut scratch.header, false)?;
        let exp = numeric(&scratch.payload, fields[0])?;
        let now = now_s as f64;
        if exp <= now {
            return Err(AuthError::Invalid);
        }
        if let Some(nbf) = fields[1]
            && numeric(&scratch.payload, Some(nbf))? > now
        {
            return Err(AuthError::Invalid);
        }
        let Some(Token::String(sub)) = fields[2] else {
            return Err(AuthError::Payload);
        };
        let Some(Token::String(name)) = fields[3] else {
            return Err(AuthError::Payload);
        };
        let mut id = 0_i64;
        if sub.start == sub.end {
            return Err(AuthError::Payload);
        }
        if sub.escaped {
            let mut at = sub.start;
            while at < sub.end {
                let c = scalar(&scratch.payload, &mut at)?;
                if !c.is_ascii_digit() {
                    return Err(AuthError::Payload);
                }
                id = id
                    .checked_mul(10)
                    .and_then(|n| n.checked_add(i64::from(u32::from(c) - u32::from('0'))))
                    .ok_or(AuthError::Payload)?;
            }
        } else {
            // Text without escapes reads as its bytes; a non-ASCII byte is no digit either way.
            let digits = scratch
                .payload
                .get(sub.start..sub.end)
                .expect("the validator recorded the string's range within payload");
            for &byte in digits {
                if !byte.is_ascii_digit() {
                    return Err(AuthError::Payload);
                }
                id = id
                    .checked_mul(10)
                    .and_then(|n| n.checked_add(i64::from(byte - b'0')))
                    .ok_or(AuthError::Payload)?;
            }
        }
        let id = NonZeroI64::new(id)
            .filter(|id| id.get() > 0)
            .ok_or(AuthError::Payload)?;
        let mut end = name.end;
        if name.escaped {
            let mut read = name.start;
            let mut write = name.start;
            while read < name.end {
                let c = scalar(&scratch.payload, &mut read)?;
                let mut encoded = [0; 4];
                let bytes = c.encode_utf8(&mut encoded).as_bytes();
                scratch
                    .payload
                    .get_mut(write..write + bytes.len())
                    .expect("the decoded username never outruns its encoded span")
                    .copy_from_slice(bytes);
                write += bytes.len();
            }
            end = write;
        }
        scratch.header.clear();
        scratch.accepted = Some(Accepted {
            user_id: id,
            username: name.start..end,
        });
        Ok(())
    }
}

fn numeric(bytes: &[u8], token: Option<Token>) -> Result<f64, AuthError> {
    let Some(Token::Number { start, end }) = token else {
        return Err(AuthError::Invalid);
    };
    let text = bytes
        .get(start..end)
        .expect("the validator recorded the number's range within bytes");
    let value = std::str::from_utf8(text)
        .map_err(|_| AuthError::Invalid)?
        .parse::<f64>()
        .map_err(|_| AuthError::Invalid)?;
    if value.is_finite() {
        Ok(value)
    } else {
        Err(AuthError::Invalid)
    }
}

fn decoded_len(len: usize) -> Result<usize, AuthError> {
    let tail = match len % 4 {
        0 => 0,
        2 => 1,
        3 => 2,
        _ => return Err(AuthError::Invalid),
    };
    let count = (len / 4)
        .checked_mul(3)
        .and_then(|n| n.checked_add(tail))
        .ok_or(AuthError::Invalid)?;
    if count > DECODE_MAX {
        Err(AuthError::Invalid)
    } else {
        Ok(count)
    }
}

fn digit(byte: u8) -> Result<u8, AuthError> {
    match byte {
        b'A'..=b'Z' => Ok(byte - b'A'),
        b'a'..=b'z' => Ok(byte - b'a' + 26),
        b'0'..=b'9' => Ok(byte - b'0' + 52),
        b'-' => Ok(62),
        b'_' => Ok(63),
        _ => Err(AuthError::Invalid),
    }
}

#[cfg(test)]
fn decode(bytes: &[u8], out: &mut Vec<u8>) -> Result<(), AuthError> {
    decode_scan(bytes, out, |_, _| {})
}

fn decode_scan(
    bytes: &[u8],
    out: &mut Vec<u8>,
    mut scan: impl FnMut(&[u8], usize),
) -> Result<(), AuthError> {
    decoded_len(bytes.len())?;
    out.clear();
    // Four characters carry three bytes; a tail of two or three characters carries one or two,
    // and its spare low bits must be zero (canonical base64url without padding).
    let mut quads = bytes.chunks_exact(4);
    for quad in &mut quads {
        let &[a, b, c, d] = quad else {
            return Err(AuthError::Invalid);
        };
        let bits = (u32::from(digit(a)?) << 18)
            | (u32::from(digit(b)?) << 12)
            | (u32::from(digit(c)?) << 6)
            | u32::from(digit(d)?);
        let [_, first, second, third] = bits.to_be_bytes();
        out.extend_from_slice(&[first, second, third]);
        for at in out.len() - 3..out.len() {
            scan(out, at);
        }
    }
    match *quads.remainder() {
        [] => {}
        [a, b] => {
            let bits = (u32::from(digit(a)?) << 6) | u32::from(digit(b)?);
            if bits & 0xf != 0 {
                return Err(AuthError::Invalid);
            }
            let [_, _, _, first] = (bits >> 4).to_be_bytes();
            out.push(first);
            scan(out, out.len() - 1);
        }
        [a, b, c] => {
            let bits =
                (u32::from(digit(a)?) << 12) | (u32::from(digit(b)?) << 6) | u32::from(digit(c)?);
            if bits & 0x3 != 0 {
                return Err(AuthError::Invalid);
            }
            let [_, _, first, second] = (bits >> 2).to_be_bytes();
            out.extend_from_slice(&[first, second]);
            scan(out, out.len() - 2);
            scan(out, out.len() - 1);
        }
        _ => return Err(AuthError::Invalid),
    }
    Ok(())
}

fn encode_signature(bytes: &[u8]) -> [u8; 43] {
    let mut out = [0; 43];
    let mut at = 0;
    let mut accumulator = 0_u32;
    let mut bits = 0;
    for &byte in bytes {
        accumulator = (accumulator << 8) | u32::from(byte);
        bits += 8;
        while bits >= 6 {
            bits -= 6;
            let index = usize::try_from((accumulator >> bits) & 63).expect("six bits");
            let character = *ALPHABET.get(index).expect("a six-bit index is below 64");
            *out.get_mut(at)
                .expect("a 32-byte digest encodes to exactly 43 characters") = character;
            at += 1;
        }
        accumulator &= (1 << bits) - 1;
    }
    if bits != 0 {
        let index = usize::try_from(accumulator << (6 - bits)).expect("six bits");
        let character = *ALPHABET.get(index).expect("a six-bit index is below 64");
        *out.get_mut(at)
            .expect("a 32-byte digest encodes to exactly 43 characters") = character;
    }
    out
}

#[derive(Clone, Copy)]
struct Text {
    start: usize,
    end: usize,
    escaped: bool,
    keys: u8,
    hs256: bool,
}
#[derive(Clone, Copy)]
enum Token {
    String(Text),
    Number { start: usize, end: usize },
    Other,
}

// The common flat ASCII object is recognized as decoded triples arrive. Containers, escapes
// and Unicode use the same full validator below, never a more permissive token parser.
#[derive(Clone, Copy, PartialEq)]
enum ScanState {
    Open,
    First,
    Key,
    KeyText,
    Colon,
    Value,
    Text,
    Atom,
    After,
    Done,
    General,
}

struct Scan {
    state: ScanState,
    start: usize,
    keys: u8,
    hs256: bool,
    key: Option<usize>,
    seen: u8,
    fields: [Option<Token>; 4],
}

impl Scan {
    fn new() -> Self {
        Self {
            state: ScanState::Open,
            start: 0,
            keys: 0,
            hs256: false,
            key: None,
            seen: 0,
            fields: [None; 4],
        }
    }
    fn capture(&mut self, token: Token) {
        if let Some(key) = self.key {
            *self.fields.get_mut(key).expect("recognized key below four") = Some(token);
        }
    }
    fn byte(&mut self, bytes: &[u8], at: usize, header: bool) {
        const KEYS: [&[u8]; 5] = [b"alg", b"exp", b"nbf", b"sub", b"username"];
        let byte = *bytes.get(at).expect("the decoder just wrote this byte");
        let white = matches!(byte, b' ' | b'\t' | b'\r' | b'\n');
        let mut state = self.state;
        if state == ScanState::Atom && (white || matches!(byte, b',' | b'}')) {
            let atom = bytes
                .get(self.start..at)
                .expect("atom inside decoded prefix");
            let token = if matches!(atom, b"true" | b"false" | b"null") {
                Some(Token::Other)
            } else {
                let mut end = self.start;
                (number(bytes, &mut end).is_ok() && end == at).then_some(Token::Number {
                    start: self.start,
                    end: at,
                })
            };
            let Some(token) = token else {
                self.state = ScanState::General;
                return;
            };
            self.capture(token);
            state = ScanState::After;
        }
        if white
            && !matches!(
                state,
                ScanState::KeyText | ScanState::Text | ScanState::Atom
            )
        {
            self.state = state;
            return;
        }
        self.state = match state {
            ScanState::Open if byte == b'{' => ScanState::First,
            ScanState::First if byte == b'}' => ScanState::Done,
            ScanState::First | ScanState::Key if byte == b'"' => {
                self.start = at + 1;
                self.keys = 31;
                ScanState::KeyText
            }
            ScanState::KeyText if byte == b'"' => {
                let count = at - self.start;
                for (index, key) in KEYS.iter().enumerate() {
                    if key.len() != count {
                        self.keys &= !(1 << index);
                    }
                }
                self.key = if header {
                    (self.keys == 1).then_some(0)
                } else {
                    match self.keys {
                        2 => Some(0),
                        4 => Some(1),
                        8 => Some(2),
                        16 => Some(3),
                        _ => None,
                    }
                };
                if let Some(key) = self.key {
                    if self.seen & (1 << key) != 0 {
                        return self.state = ScanState::General;
                    }
                    self.seen |= 1 << key;
                }
                ScanState::Colon
            }
            ScanState::KeyText if (32..128).contains(&byte) && byte != b'\\' => {
                for (index, key) in KEYS.iter().enumerate() {
                    if key.get(at - self.start) != Some(&byte) {
                        self.keys &= !(1 << index);
                    }
                }
                ScanState::KeyText
            }
            ScanState::Colon if byte == b':' => ScanState::Value,
            ScanState::Value if byte == b'"' => {
                self.start = at + 1;
                self.hs256 = true;
                ScanState::Text
            }
            ScanState::Value if matches!(byte, b'-' | b'0'..=b'9' | b't' | b'f' | b'n') => {
                self.start = at;
                ScanState::Atom
            }
            ScanState::Text if byte == b'"' => {
                self.capture(Token::String(Text {
                    start: self.start,
                    end: at,
                    escaped: false,
                    keys: 0,
                    hs256: self.hs256 && at - self.start == 5,
                }));
                ScanState::After
            }
            ScanState::Text if (32..128).contains(&byte) && byte != b'\\' => {
                self.hs256 &= b"HS256".get(at - self.start) == Some(&byte);
                ScanState::Text
            }
            ScanState::Atom if byte.is_ascii() && !matches!(byte, b'{' | b'[' | b'"' | b'\\') => {
                ScanState::Atom
            }
            ScanState::After if byte == b',' => ScanState::Key,
            ScanState::After if byte == b'}' => ScanState::Done,
            _ => ScanState::General,
        };
    }
}

fn decode_document(
    bytes: &[u8],
    out: &mut Vec<u8>,
    stack: &mut Vec<u8>,
    header: bool,
) -> Result<[Option<Token>; 4], AuthError> {
    let mut scan = Scan::new();
    decode_scan(bytes, out, |bytes, at| {
        if scan.state != ScanState::General {
            scan.byte(bytes, at, header);
        }
    })?;
    if scan.state == ScanState::Done {
        Ok(scan.fields)
    } else {
        document(out, stack, header)
    }
}

fn ws(bytes: &[u8], at: &mut usize) {
    while bytes
        .get(*at)
        .is_some_and(|b| matches!(b, b' ' | b'\t' | b'\r' | b'\n'))
    {
        *at += 1;
    }
}

// The validator's grammar contexts; each open container keeps one discriminant byte on the
// decode buffer that serves as its stack.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
enum State {
    ObjectFirst = 0,
    ObjectNext = 1,
    ObjectColon = 2,
    ObjectValue = 3,
    ObjectComma = 4,
    ArrayFirst = 5,
    ArrayNext = 6,
    ArrayComma = 7,
}

impl From<State> for u8 {
    fn from(state: State) -> Self {
        // The enum is repr(u8), so its discriminant is the stack byte.
        state as Self
    }
}

impl TryFrom<u8> for State {
    type Error = u8;

    fn try_from(byte: u8) -> Result<Self, u8> {
        match byte {
            0 => Ok(Self::ObjectFirst),
            1 => Ok(Self::ObjectNext),
            2 => Ok(Self::ObjectColon),
            3 => Ok(Self::ObjectValue),
            4 => Ok(Self::ObjectComma),
            5 => Ok(Self::ArrayFirst),
            6 => Ok(Self::ArrayNext),
            7 => Ok(Self::ArrayComma),
            rejected => Err(rejected),
        }
    }
}

fn document(
    bytes: &[u8],
    stack: &mut Vec<u8>,
    header: bool,
) -> Result<[Option<Token>; 4], AuthError> {
    stack.clear();
    let mut at = 0;
    ws(bytes, &mut at);
    if bytes.get(at) != Some(&b'{') {
        return Err(AuthError::Invalid);
    }
    at += 1;
    stack.push(u8::from(State::ObjectFirst));
    let mut fields = [None; 4];
    let mut key = None;
    let mut seen = 0_u8;
    while let Some(&top) = stack.last() {
        ws(bytes, &mut at);
        let state = State::try_from(top).expect("the stack holds only State discriminants");
        match state {
            State::ObjectFirst | State::ObjectNext => {
                if state == State::ObjectFirst && bytes.get(at) == Some(&b'}') {
                    at += 1;
                    stack.pop();
                } else {
                    let text = string(bytes, &mut at)?;
                    if stack.len() == 1 {
                        key = if header {
                            (text.keys & 1 != 0).then_some(0)
                        } else {
                            match text.keys {
                                2 => Some(0),
                                4 => Some(1),
                                8 => Some(2),
                                16 => Some(3),
                                _ => None,
                            }
                        };
                        if let Some(index) = key {
                            if seen & (1 << index) != 0 {
                                return Err(AuthError::Invalid);
                            }
                            seen |= 1 << index;
                        }
                    }
                    *stack.last_mut().expect("open object") = u8::from(State::ObjectColon);
                }
            }
            State::ObjectColon => {
                if bytes.get(at) != Some(&b':') {
                    return Err(AuthError::Invalid);
                }
                at += 1;
                *stack.last_mut().expect("open object") = u8::from(State::ObjectValue);
            }
            State::ObjectValue => {
                let capture = if stack.len() == 1 { key } else { None };
                *stack.last_mut().expect("open object") = u8::from(State::ObjectComma);
                let token = value(bytes, &mut at, stack)?;
                if let Some(index) = capture {
                    *fields
                        .get_mut(index)
                        .expect("a recognized key maps to one of the four fields") = Some(token);
                }
            }
            State::ObjectComma => match bytes.get(at) {
                Some(b',') => {
                    at += 1;
                    *stack.last_mut().expect("open object") = u8::from(State::ObjectNext);
                }
                Some(b'}') => {
                    at += 1;
                    stack.pop();
                }
                _ => return Err(AuthError::Invalid),
            },
            State::ArrayFirst | State::ArrayNext => {
                if state == State::ArrayFirst && bytes.get(at) == Some(&b']') {
                    at += 1;
                    stack.pop();
                } else {
                    *stack.last_mut().expect("open array") = u8::from(State::ArrayComma);
                    value(bytes, &mut at, stack)?;
                }
            }
            State::ArrayComma => match bytes.get(at) {
                Some(b',') => {
                    at += 1;
                    *stack.last_mut().expect("open array") = u8::from(State::ArrayNext);
                }
                Some(b']') => {
                    at += 1;
                    stack.pop();
                }
                _ => return Err(AuthError::Invalid),
            },
        }
    }
    ws(bytes, &mut at);
    if at != bytes.len() {
        return Err(AuthError::Invalid);
    }
    Ok(fields)
}

fn value(bytes: &[u8], at: &mut usize, stack: &mut Vec<u8>) -> Result<Token, AuthError> {
    ws(bytes, at);
    match bytes.get(*at) {
        Some(b'{') => {
            *at += 1;
            stack.push(u8::from(State::ObjectFirst));
        }
        Some(b'[') => {
            *at += 1;
            stack.push(u8::from(State::ArrayFirst));
        }
        Some(b'"') => return string(bytes, at).map(Token::String),
        Some(b't') => literal(bytes, at, b"true")?,
        Some(b'f') => literal(bytes, at, b"false")?,
        Some(b'n') => literal(bytes, at, b"null")?,
        Some(b'-' | b'0'..=b'9') => {
            let start = *at;
            number(bytes, at)?;
            return Ok(Token::Number { start, end: *at });
        }
        _ => return Err(AuthError::Invalid),
    }
    Ok(Token::Other)
}

fn literal(bytes: &[u8], at: &mut usize, text: &[u8]) -> Result<(), AuthError> {
    if bytes.get(*at..*at + text.len()) != Some(text) {
        return Err(AuthError::Invalid);
    }
    *at += text.len();
    Ok(())
}
fn digits(bytes: &[u8], at: &mut usize) -> bool {
    let start = *at;
    while bytes.get(*at).is_some_and(u8::is_ascii_digit) {
        *at += 1;
    }
    *at > start
}
fn number(bytes: &[u8], at: &mut usize) -> Result<(), AuthError> {
    if bytes.get(*at) == Some(&b'-') {
        *at += 1;
    }
    match bytes.get(*at) {
        Some(b'0') => *at += 1,
        Some(b'1'..=b'9') => {
            *at += 1;
            digits(bytes, at);
        }
        _ => return Err(AuthError::Invalid),
    }
    if bytes.get(*at) == Some(&b'.') {
        *at += 1;
        if !digits(bytes, at) {
            return Err(AuthError::Invalid);
        }
    }
    if bytes.get(*at).is_some_and(|b| matches!(b, b'e' | b'E')) {
        *at += 1;
        if bytes.get(*at).is_some_and(|b| matches!(b, b'+' | b'-')) {
            *at += 1;
        }
        if !digits(bytes, at) {
            return Err(AuthError::Invalid);
        }
    }
    Ok(())
}

fn string(bytes: &[u8], at: &mut usize) -> Result<Text, AuthError> {
    const KEYS: [&[u8]; 5] = [b"alg", b"exp", b"nbf", b"sub", b"username"];
    if bytes.get(*at) != Some(&b'"') {
        return Err(AuthError::Invalid);
    }
    *at += 1;
    let start = *at;
    // Plain ASCII text without escapes decodes to itself, so its key and HS256 tests compare its
    // bytes; any other string takes the scalar loop below.
    let plain = bytes.get(start..).map_or(0, |rest| {
        rest.iter()
            .take_while(|&&b| (0x20..0x80).contains(&b) && b != b'"' && b != b'\\')
            .count()
    });
    if bytes.get(start + plain) == Some(&b'"') {
        let text = bytes
            .get(start..start + plain)
            .expect("the plain run lies inside bytes");
        let mut matches = 0_u8;
        for (index, key) in KEYS.iter().enumerate() {
            if text == *key {
                matches |= 1 << index;
            }
        }
        *at = start + plain + 1;
        return Ok(Text {
            start,
            end: start + plain,
            escaped: false,
            keys: matches,
            hs256: text == b"HS256",
        });
    }
    let mut escaped = false;
    let mut matches = 31_u8;
    let mut hs256 = true;
    let mut count = 0;
    while bytes.get(*at) != Some(&b'"') {
        escaped |= bytes.get(*at) == Some(&b'\\');
        let c = scalar(bytes, at)?;
        for (index, key) in KEYS.iter().enumerate() {
            if key.get(count).is_none_or(|&b| c != char::from(b)) {
                matches &= !(1 << index);
            }
        }
        hs256 &= b"HS256".get(count).is_some_and(|&b| c == char::from(b));
        count += 1;
    }
    for (index, key) in KEYS.iter().enumerate() {
        if key.len() != count {
            matches &= !(1 << index);
        }
    }
    let end = *at;
    *at += 1;
    Ok(Text {
        start,
        end,
        escaped,
        keys: matches,
        hs256: hs256 && count == 5,
    })
}

fn scalar(bytes: &[u8], at: &mut usize) -> Result<char, AuthError> {
    let byte = *bytes.get(*at).ok_or(AuthError::Invalid)?;
    if byte == b'\\' {
        *at += 1;
        let escape = *bytes.get(*at).ok_or(AuthError::Invalid)?;
        *at += 1;
        return match escape {
            b'"' => Ok('"'),
            b'\\' => Ok('\\'),
            b'/' => Ok('/'),
            b'b' => Ok('\u{0008}'),
            b'f' => Ok('\u{000c}'),
            b'n' => Ok('\n'),
            b'r' => Ok('\r'),
            b't' => Ok('\t'),
            b'u' => {
                let first = hex4(bytes, at)?;
                let code = if (0xd800..=0xdbff).contains(&first) {
                    literal(bytes, at, b"\\u")?;
                    let second = hex4(bytes, at)?;
                    if !(0xdc00..=0xdfff).contains(&second) {
                        return Err(AuthError::Invalid);
                    }
                    0x10000 + (first - 0xd800) * 1024 + second - 0xdc00
                } else {
                    first
                };
                char::from_u32(code).ok_or(AuthError::Invalid)
            }
            _ => Err(AuthError::Invalid),
        };
    }
    if byte < 32 || byte == b'"' {
        return Err(AuthError::Invalid);
    }
    let width = match byte {
        0..=0x7f => 1,
        0xc2..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf4 => 4,
        _ => return Err(AuthError::Invalid),
    };
    let text = std::str::from_utf8(bytes.get(*at..*at + width).ok_or(AuthError::Invalid)?)
        .map_err(|_| AuthError::Invalid)?;
    *at += width;
    text.chars().next().ok_or(AuthError::Invalid)
}
fn hex4(bytes: &[u8], at: &mut usize) -> Result<u32, AuthError> {
    let mut value = 0;
    for _ in 0..4 {
        let digit = match bytes.get(*at) {
            Some(b @ b'0'..=b'9') => b - b'0',
            Some(b @ b'a'..=b'f') => b - b'a' + 10,
            Some(b @ b'A'..=b'F') => b - b'A' + 10,
            _ => return Err(AuthError::Invalid),
        };
        value = value * 16 + u32::from(digit);
        *at += 1;
    }
    Ok(value)
}

#[cfg(test)]
mod fusion_tests {
    use super::*;

    fn encoded(bytes: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        let mut bits = 0_u32;
        let mut count = 0;
        for &byte in bytes {
            bits = (bits << 8) | u32::from(byte);
            count += 8;
            while count >= 6 {
                count -= 6;
                out.push(ALPHABET[((bits >> count) & 63) as usize]);
            }
            bits &= (1 << count) - 1;
        }
        if count != 0 {
            out.push(ALPHABET[(bits << (6 - count)) as usize]);
        }
        out
    }

    fn shape(field: Option<Token>) -> Option<(u8, usize, usize, bool, bool)> {
        field.map(|field| match field {
            Token::String(text) => (1, text.start, text.end, text.escaped, text.hs256),
            Token::Number { start, end } => (2, start, end, false, false),
            Token::Other => (3, 0, 0, false, false),
        })
    }

    /// CORE: catches a fused decoder accepting different JSON or claims from the full validator.
    #[test]
    fn fused_documents_match_full_validator() {
        let fixtures = [
            r#"{"alg":"HS256","typ":"JWT"}"#,
            r#"{"sub":"42","username":"alice","exp":2000000000}"#,
            r#"{"exp":2e9,"nbf":0,"sub":"00042","username":""}"#,
            r#"{"exp":-0.5e+3,"sub":"1","username":"sub","other":true}"#,
            r#"{"sub":"1","sub":"2","exp":2e9,"username":"x"}"#,
            r#"{"exp":01,"sub":"1","username":"x"}"#,
            r#"{"exp":2e,"sub":"1","username":"x"}"#,
            r#"{"exp":2e9,"sub":"1","username":"caf\u00e9","extra":[{"x":null}]}"#,
            r#"{"\u0061lg":"HS256"}"#,
            r#"{"alg":"HS256",}"#,
            r#"{}false"#,
            r#"{"exp":2e9,"sub":"1","username":"é"}"#,
        ];
        for fixture in fixtures {
            for padding in 0..=12 {
                let bytes = format!("{}{}{}", " ".repeat(padding), fixture, "\n".repeat(padding));
                let wire = encoded(bytes.as_bytes());
                let mut decoded = Vec::new();
                decode(&wire, &mut decoded).unwrap();
                assert_eq!(decoded, bytes.as_bytes());
                for header in [false, true] {
                    let expected =
                        document(&decoded, &mut Vec::new(), header).map(|v| v.map(shape));
                    let actual = decode_document(&wire, &mut Vec::new(), &mut Vec::new(), header)
                        .map(|v| v.map(shape));
                    assert_eq!(actual, expected, "{bytes}");
                }
            }
        }
        for len in 0..=256 {
            let bytes: Vec<u8> = (0..len).map(|at| u8::try_from(at % 256).unwrap()).collect();
            let wire = encoded(&bytes);
            let mut decoded = Vec::new();
            decode(&wire, &mut decoded).unwrap();
            assert_eq!(decoded, bytes);
        }
    }
}
