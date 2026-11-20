use std::fmt;
use std::ops::Range;

const CAP: usize = 65_536;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum BodyError {
    Malformed,
    Required,
    TooLong,
}

impl fmt::Display for BodyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Malformed => "malformed JSON body",
            Self::Required => "body is required",
            Self::TooLong => "body must be at most 500 characters",
        })
    }
}

impl std::error::Error for BodyError {}

/// # Errors
/// Returns Malformed for invalid JSON, Required for a missing or empty string,
/// and TooLong for more than 500 trimmed Unicode scalars.
pub(crate) fn parse_body<'a>(
    bytes: &'a [u8],
    scratch: &'a mut Vec<u8>,
) -> Result<&'a str, BodyError> {
    scratch.clear();
    let selected = if bytes.len() <= CAP {
        if scratch.capacity() < bytes.len() {
            scratch.reserve_exact(bytes.len());
        }
        validate(bytes, scratch)
    } else {
        Err(BodyError::Malformed)
    };
    scratch.clear();
    let selected = selected?;
    let Some(selected) = selected else {
        return Err(BodyError::Required);
    };
    let text = if selected.escaped {
        let mut at = selected.range.start;
        while at < selected.range.end {
            let scalar = scalar(bytes, &mut at).expect("validated selected string");
            let mut encoded = [0; 4];
            scratch.extend_from_slice(scalar.encode_utf8(&mut encoded).as_bytes());
        }
        std::str::from_utf8(scratch).expect("decoded Unicode scalars")
    } else {
        std::str::from_utf8(
            bytes
                .get(selected.range.clone())
                .expect("validate returned a range within bytes"),
        )
        .expect("validated UTF-8 string")
    };
    let trimmed = text.trim_matches(trim_scalar);
    let error = if trimmed.is_empty() {
        Some(BodyError::Required)
    } else if trimmed.chars().count() > 500 {
        Some(BodyError::TooLong)
    } else {
        None
    };
    if let Some(error) = error {
        scratch.clear();
        return Err(error);
    }
    // Reborrow only after the error path has finished mutating scratch.
    if selected.escaped {
        Ok(std::str::from_utf8(scratch)
            .expect("decoded Unicode scalars")
            .trim_matches(trim_scalar))
    } else {
        Ok(std::str::from_utf8(
            bytes
                .get(selected.range)
                .expect("validate returned a range within bytes"),
        )
        .expect("validated UTF-8 string")
        .trim_matches(trim_scalar))
    }
}

fn trim_scalar(c: char) -> bool {
    matches!(c, '\u{0009}'..='\u{000d}' | '\u{0020}' | '\u{0085}' | '\u{00a0}' | '\u{1680}'
        | '\u{2000}'..='\u{200a}' | '\u{2028}' | '\u{2029}' | '\u{202f}' | '\u{205f}' | '\u{3000}' | '\u{feff}')
}

struct Text {
    range: Range<usize>,
    escaped: bool,
    body_key: bool,
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
// caller's scratch stack.
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

fn validate(bytes: &[u8], stack: &mut Vec<u8>) -> Result<Option<Text>, BodyError> {
    let mut at = 0;
    ws(bytes, &mut at);
    let root_object = bytes.get(at) == Some(&b'{');
    value(bytes, &mut at, stack)?;
    let mut selected = None;
    let mut body_key = false;
    while let Some(&top) = stack.last() {
        ws(bytes, &mut at);
        let state = State::try_from(top).expect("the stack holds only State discriminants");
        match state {
            State::ObjectFirst | State::ObjectNext => {
                if state == State::ObjectFirst && bytes.get(at) == Some(&b'}') {
                    at += 1;
                    stack.pop();
                } else {
                    let key = string(bytes, &mut at)?;
                    if root_object && stack.len() == 1 {
                        body_key = key.body_key;
                    }
                    *stack.last_mut().expect("open object") = u8::from(State::ObjectColon);
                }
            }
            State::ObjectColon => {
                if bytes.get(at) != Some(&b':') {
                    return Err(BodyError::Malformed);
                }
                at += 1;
                *stack.last_mut().expect("open object") = u8::from(State::ObjectValue);
            }
            State::ObjectValue => {
                let capture = root_object && stack.len() == 1 && body_key;
                *stack.last_mut().expect("open object") = u8::from(State::ObjectComma);
                let text = value(bytes, &mut at, stack)?;
                if capture {
                    selected = text;
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
                _ => return Err(BodyError::Malformed),
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
                _ => return Err(BodyError::Malformed),
            },
        }
    }
    ws(bytes, &mut at);
    if at != bytes.len() {
        return Err(BodyError::Malformed);
    }
    Ok(selected)
}

fn value(bytes: &[u8], at: &mut usize, stack: &mut Vec<u8>) -> Result<Option<Text>, BodyError> {
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
        Some(b'"') => return string(bytes, at).map(Some),
        Some(b't') => literal(bytes, at, b"true")?,
        Some(b'f') => literal(bytes, at, b"false")?,
        Some(b'n') => literal(bytes, at, b"null")?,
        Some(b'-' | b'0'..=b'9') => number(bytes, at)?,
        _ => return Err(BodyError::Malformed),
    }
    Ok(None)
}

fn literal(bytes: &[u8], at: &mut usize, text: &[u8]) -> Result<(), BodyError> {
    if bytes.get(*at..*at + text.len()) != Some(text) {
        return Err(BodyError::Malformed);
    }
    *at += text.len();
    Ok(())
}

fn number(bytes: &[u8], at: &mut usize) -> Result<(), BodyError> {
    if bytes.get(*at) == Some(&b'-') {
        *at += 1;
    }
    match bytes.get(*at) {
        Some(b'0') => *at += 1,
        Some(b'1'..=b'9') => {
            *at += 1;
            digits(bytes, at);
        }
        _ => return Err(BodyError::Malformed),
    }
    if bytes.get(*at) == Some(&b'.') {
        *at += 1;
        if !digits(bytes, at) {
            return Err(BodyError::Malformed);
        }
    }
    if bytes.get(*at).is_some_and(|b| matches!(b, b'e' | b'E')) {
        *at += 1;
        if bytes.get(*at).is_some_and(|b| matches!(b, b'+' | b'-')) {
            *at += 1;
        }
        if !digits(bytes, at) {
            return Err(BodyError::Malformed);
        }
    }
    Ok(())
}

fn digits(bytes: &[u8], at: &mut usize) -> bool {
    let start = *at;
    while bytes.get(*at).is_some_and(u8::is_ascii_digit) {
        *at += 1;
    }
    *at > start
}

fn string(bytes: &[u8], at: &mut usize) -> Result<Text, BodyError> {
    if bytes.get(*at) != Some(&b'"') {
        return Err(BodyError::Malformed);
    }
    *at += 1;
    let start = *at;
    let mut escaped = false;
    let mut matched = true;
    let mut key_len = 0;
    while bytes.get(*at) != Some(&b'"') {
        escaped |= bytes.get(*at) == Some(&b'\\');
        let c = scalar(bytes, at)?;
        matched &= b"body".get(key_len).is_some_and(|&b| c == char::from(b));
        key_len += 1;
    }
    let end = *at;
    *at += 1;
    Ok(Text {
        range: start..end,
        escaped,
        body_key: matched && key_len == 4,
    })
}

fn scalar(bytes: &[u8], at: &mut usize) -> Result<char, BodyError> {
    let b = *bytes.get(*at).ok_or(BodyError::Malformed)?;
    if b == b'\\' {
        *at += 1;
        let escape = *bytes.get(*at).ok_or(BodyError::Malformed)?;
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
                        return Err(BodyError::Malformed);
                    }
                    0x10000 + (first - 0xd800) * 1024 + second - 0xdc00
                } else {
                    first
                };
                char::from_u32(code).ok_or(BodyError::Malformed)
            }
            _ => Err(BodyError::Malformed),
        };
    }
    if b < 32 || b == b'"' {
        return Err(BodyError::Malformed);
    }
    let width = match b {
        0..=0x7f => 1,
        0xc2..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf4 => 4,
        _ => return Err(BodyError::Malformed),
    };
    let text = std::str::from_utf8(bytes.get(*at..*at + width).ok_or(BodyError::Malformed)?)
        .map_err(|_| BodyError::Malformed)?;
    *at += width;
    text.chars().next().ok_or(BodyError::Malformed)
}

fn hex4(bytes: &[u8], at: &mut usize) -> Result<u32, BodyError> {
    let mut n = 0;
    for _ in 0..4 {
        let digit = match bytes.get(*at) {
            Some(b @ b'0'..=b'9') => b - b'0',
            Some(b @ b'a'..=b'f') => b - b'a' + 10,
            Some(b @ b'A'..=b'F') => b - b'A' + 10,
            _ => return Err(BodyError::Malformed),
        };
        n = n * 16 + u32::from(digit);
        *at += 1;
    }
    Ok(n)
}

pub(crate) fn escape_into(out: &mut Vec<u8>, s: &[u8]) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    out.push(b'"');
    let mut at = 0;
    let mut start = 0;
    while at < s.len() {
        if s.len() - at >= 16
            // SAFETY: get(at..at + 16) yields exactly 16 bytes, all plain_block's contract asks.
            && unsafe {
                plain_block(
                    s.get(at..at + 16)
                        .expect("16 bytes remain after at, checked just before"),
                )
            }
        {
            at += 16;
            continue;
        }
        let byte = *s.get(at).expect("the loop keeps at below s.len()");
        if byte < 32 || matches!(byte, b'"' | b'\\') {
            out.extend_from_slice(s.get(start..at).expect("start <= at < s.len()"));
            match byte {
                b'"' => out.extend_from_slice(b"\\\""),
                b'\\' => out.extend_from_slice(b"\\\\"),
                b'\n' => out.extend_from_slice(b"\\n"),
                b'\r' => out.extend_from_slice(b"\\r"),
                b'\t' => out.extend_from_slice(b"\\t"),
                _ => out.extend_from_slice(&[
                    b'\\',
                    b'u',
                    b'0',
                    b'0',
                    *HEX.get(usize::from(byte >> 4))
                        .expect("a nibble is below 16"),
                    *HEX.get(usize::from(byte & 15))
                        .expect("a nibble is below 16"),
                ]),
            }
            start = at + 1;
        }
        at += 1;
    }
    out.extend_from_slice(s.get(start..).expect("start <= s.len()"));
    out.push(b'"');
}

// The one scalar predicate: targets without SSE2 and the safe build use it.
fn plain_scalar(bytes: &[u8]) -> bool {
    bytes.iter().all(|&b| b >= 32 && b != b'"' && b != b'\\')
}

/// Whether a 16-byte block holds no byte that S(x) escapes.
///
/// # Safety
/// `bytes` must be exactly 16 bytes long: the SSE2 path loads 16 bytes from its start.
#[cfg(target_arch = "x86_64")]
unsafe fn plain_block(bytes: &[u8]) -> bool {
    use std::arch::x86_64::{
        _mm_and_si128, _mm_cmpeq_epi8, _mm_loadu_si128, _mm_movemask_epi8, _mm_or_si128,
        _mm_set1_epi8, _mm_setzero_si128,
    };
    const SAFE_BUILD: bool = cfg!(twelve_safe);
    if SAFE_BUILD {
        return plain_scalar(bytes);
    }
    // SAFETY: this function's contract gives exactly 16 readable bytes; _mm_loadu_si128 reads
    // those 16 bytes at any alignment, SSE2 is in the x86_64 baseline, and no pointer is kept.
    // UNSAFE PATH (speed): unsafe code for an unmeasured speed-up; each _mm_loadu_si128 reads 16 bytes only after escape_into checked that 16 remain, the load needs no alignment and SSE2 is in the x86_64 baseline, so every load stays inside the slice
    let block = unsafe { _mm_loadu_si128(bytes.as_ptr().cast()) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let high_bits = unsafe { _mm_set1_epi8(-32) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let masked = unsafe { _mm_and_si128(block, high_bits) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let zero = unsafe { _mm_setzero_si128() };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let control = unsafe { _mm_cmpeq_epi8(masked, zero) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let quote_byte = unsafe { _mm_set1_epi8(34) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let quote = unsafe { _mm_cmpeq_epi8(block, quote_byte) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let slash_byte = unsafe { _mm_set1_epi8(92) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let slash = unsafe { _mm_cmpeq_epi8(block, slash_byte) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let escapes = unsafe { _mm_or_si128(quote, slash) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let special = unsafe { _mm_or_si128(control, escapes) };
    // SAFETY: SSE2, this intrinsic's one requirement, is in the x86_64 baseline; it touches no memory.
    let mask = unsafe { _mm_movemask_epi8(special) };
    mask == 0
}

/// Whether a 16-byte block holds no byte that S(x) escapes.
///
/// # Safety
/// `bytes` must be exactly 16 bytes long, the x86_64 form's contract, so callers are the same on
/// every target.
#[cfg(not(target_arch = "x86_64"))]
unsafe fn plain_block(bytes: &[u8]) -> bool {
    plain_scalar(bytes)
}
