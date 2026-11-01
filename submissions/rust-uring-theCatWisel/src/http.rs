use std::borrow::Cow;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ParseError;
pub type JsonResult<T> = std::result::Result<T, ParseError>;

pub struct Claims<'a> {
    pub sub: Option<Cow<'a, str>>,
    pub username: Option<Cow<'a, str>>,
    pub exp: Option<f64>,
    pub nbf: Option<f64>,
}

struct JsonCursor<'a> {
    bytes: &'a [u8],
    index: usize,
    finite_numbers: bool,
}

impl<'a> JsonCursor<'a> {
    #[inline]
    fn new(bytes: &'a [u8], finite_numbers: bool) -> Self {
        Self {
            bytes,
            index: 0,
            finite_numbers,
        }
    }

    #[inline]
    fn skip_ws(&mut self) {
        while let Some(&b) = self.bytes.get(self.index) {
            if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
                self.index += 1;
            } else {
                break;
            }
        }
    }

    #[inline]
    fn consume(&mut self, byte: u8) -> bool {
        self.skip_ws();
        if self.bytes.get(self.index) == Some(&byte) {
            self.index += 1;
            true
        } else {
            false
        }
    }

    #[inline]
    fn expect(&mut self, byte: u8) -> JsonResult<()> {
        if self.consume(byte) {
            Ok(())
        } else {
            Err(ParseError)
        }
    }

    #[inline]
    fn finish(&mut self) -> JsonResult<()> {
        self.skip_ws();
        if self.index == self.bytes.len() {
            Ok(())
        } else {
            Err(ParseError)
        }
    }

    #[inline]
    fn string(&mut self) -> JsonResult<Cow<'a, str>> {
        self.expect(b'"')?;
        let start = self.index;
        let mut at = start;
        #[cfg(target_arch = "x86_64")]
        while at + 16 <= self.bytes.len() {
            let mask = unsafe {
                use std::arch::x86_64::*;
                let v = _mm_loadu_si128(self.bytes.as_ptr().add(at).cast());
                let special = _mm_or_si128(
                    _mm_or_si128(
                        _mm_cmpeq_epi8(v, _mm_set1_epi8(34)),
                        _mm_cmpeq_epi8(v, _mm_set1_epi8(92)),
                    ),
                    _mm_cmpeq_epi8(_mm_min_epu8(v, _mm_set1_epi8(31)), v),
                );
                (_mm_movemask_epi8(special) | _mm_movemask_epi8(v)) as u32
            };
            if mask != 0 {
                at += mask.trailing_zeros() as usize;
                break;
            }
            at += 16;
        }
        while let Some(&b) = self.bytes.get(at) {
            if (32..128).contains(&b) && b != b'"' && b != b'\\' {
                at += 1;
            } else {
                break;
            }
        }
        match self.bytes.get(at) {
            Some(b'"') => {
                self.index = at + 1;
                Ok(Cow::Borrowed(unsafe {
                    std::str::from_utf8_unchecked(&self.bytes[start..at])
                }))
            }
            Some(b'\\') => {
                self.index = at;
                self.escaped_string(start)
            }
            Some(128..=255) => self.utf8_string(start, at),
            _ => Err(ParseError),
        }
    }

    #[cold]
    fn utf8_string(&mut self, start: usize, at: usize) -> JsonResult<Cow<'a, str>> {
        let mut end = at;
        while end < self.bytes.len() {
            let b = self.bytes[end];
            if b == b'"' || b == b'\\' {
                break;
            }
            if b < 0x20 {
                return Err(ParseError);
            }
            end += 1;
        }
        if end >= self.bytes.len() {
            return Err(ParseError);
        }
        let raw = &self.bytes[start..end];
        if self.bytes[end] == b'"' {
            self.index = end + 1;
            return std::str::from_utf8(raw)
                .map(Cow::Borrowed)
                .map_err(|_| ParseError);
        }
        self.index = end;
        self.escaped_string(start)
    }

    #[cold]
    fn escaped_string(&mut self, start: usize) -> JsonResult<Cow<'a, str>> {
        let mut output = self.bytes[start..self.index].to_vec();
        loop {
            let byte = *self.bytes.get(self.index).ok_or(ParseError)?;
            self.index += 1;
            match byte {
                b'"' => {
                    return String::from_utf8(output)
                        .map(Cow::Owned)
                        .map_err(|_| ParseError);
                }
                b'\\' => {
                    let escape = *self.bytes.get(self.index).ok_or(ParseError)?;
                    self.index += 1;
                    match escape {
                        b'"' | b'\\' | b'/' => output.push(escape),
                        b'b' => output.push(8),
                        b'f' => output.push(12),
                        b'n' => output.push(b'\n'),
                        b'r' => output.push(b'\r'),
                        b't' => output.push(b'\t'),
                        b'u' => {
                            let mut code = self.hex4()?;
                            if (0xd800..=0xdbff).contains(&code) {
                                if self.bytes.get(self.index..self.index + 2) != Some(b"\\u") {
                                    return Err(ParseError);
                                }
                                self.index += 2;
                                let low = self.hex4()?;
                                if !(0xdc00..=0xdfff).contains(&low) {
                                    return Err(ParseError);
                                }
                                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                            }
                            let ch = char::from_u32(code).ok_or(ParseError)?;
                            let mut tmp = [0u8; 4];
                            output.extend_from_slice(ch.encode_utf8(&mut tmp).as_bytes());
                        }
                        _ => return Err(ParseError),
                    }
                }
                0..=31 => return Err(ParseError),
                _ => output.push(byte),
            }
        }
    }

    fn hex4(&mut self) -> JsonResult<u32> {
        let raw = self
            .bytes
            .get(self.index..self.index + 4)
            .ok_or(ParseError)?;
        let mut code = 0u32;
        for &b in raw {
            code = (code << 4)
                | match b {
                    b'0'..=b'9' => u32::from(b - b'0'),
                    b'a'..=b'f' => u32::from(b - b'a' + 10),
                    b'A'..=b'F' => u32::from(b - b'A' + 10),
                    _ => return Err(ParseError),
                };
        }
        self.index += 4;
        Ok(code)
    }

    fn literal(&mut self, text: &[u8]) -> JsonResult<()> {
        self.skip_ws();
        if self.bytes.get(self.index..self.index + text.len()) != Some(text) {
            return Err(ParseError);
        }
        self.index += text.len();
        Ok(())
    }

    fn number_bytes(&mut self) -> JsonResult<&'a [u8]> {
        self.skip_ws();
        let start = self.index;
        if self.bytes.get(self.index) == Some(&b'-') {
            self.index += 1;
        }
        match self.bytes.get(self.index) {
            Some(b'0') => self.index += 1,
            Some(b'1'..=b'9') => {
                self.index += 1;
                while self.bytes.get(self.index).is_some_and(u8::is_ascii_digit) {
                    self.index += 1;
                }
            }
            _ => return Err(ParseError),
        }
        if self.bytes.get(self.index) == Some(&b'.') {
            self.index += 1;
            self.digits()?;
        }
        if matches!(self.bytes.get(self.index), Some(b'e' | b'E')) {
            self.index += 1;
            if matches!(self.bytes.get(self.index), Some(b'+' | b'-')) {
                self.index += 1;
            }
            self.digits()?;
        }
        Ok(&self.bytes[start..self.index])
    }

    fn digits(&mut self) -> JsonResult<()> {
        let start = self.index;
        while self.bytes.get(self.index).is_some_and(u8::is_ascii_digit) {
            self.index += 1;
        }
        if start != self.index {
            Ok(())
        } else {
            Err(ParseError)
        }
    }

    fn number(&mut self) -> JsonResult<f64> {
        let raw = self.number_bytes()?;
        let s = unsafe { std::str::from_utf8_unchecked(raw) };
        let value: f64 = s.parse().map_err(|_| ParseError)?;
        if value.is_finite() {
            Ok(value)
        } else {
            Err(ParseError)
        }
    }

    fn optional_number(&mut self) -> JsonResult<Option<f64>> {
        self.skip_ws();
        if self.bytes.get(self.index) == Some(&b'n') {
            self.literal(b"null")?;
            Ok(None)
        } else {
            self.number().map(Some)
        }
    }

    fn text_or_skip(&mut self) -> JsonResult<Option<Cow<'a, str>>> {
        self.skip_ws();
        if self.bytes.get(self.index) == Some(&b'"') {
            self.string().map(Some)
        } else {
            self.skip_value(1)?;
            Ok(None)
        }
    }

    #[inline]
    fn object(
        &mut self,
        depth: usize,
        mut field: impl FnMut(&mut Self, Cow<'a, str>) -> JsonResult<()>,
    ) -> JsonResult<()> {
        if depth >= 127 {
            return Err(ParseError);
        }
        self.expect(b'{')?;
        if self.consume(b'}') {
            return Ok(());
        }
        loop {
            let key = self.string()?;
            self.expect(b':')?;
            field(self, key)?;
            if self.consume(b'}') {
                return Ok(());
            }
            self.expect(b',')?;
        }
    }

    fn skip_value(&mut self, depth: usize) -> JsonResult<()> {
        self.skip_ws();
        match self.bytes.get(self.index) {
            Some(b'"') => {
                self.string()?;
            }
            Some(b'{') => self.object(depth, |cursor, _| cursor.skip_value(depth + 1))?,
            Some(b'[') => {
                if depth >= 127 {
                    return Err(ParseError);
                }
                self.index += 1;
                if self.consume(b']') {
                    return Ok(());
                }
                loop {
                    self.skip_value(depth + 1)?;
                    if self.consume(b']') {
                        break;
                    }
                    self.expect(b',')?;
                }
            }
            Some(b't') => self.literal(b"true")?,
            Some(b'f') => self.literal(b"false")?,
            Some(b'n') => self.literal(b"null")?,
            Some(b'-' | b'0'..=b'9') => {
                if self.finite_numbers {
                    self.number()?;
                } else {
                    self.number_bytes()?;
                }
            }
            _ => return Err(ParseError),
        }
        Ok(())
    }
}

pub fn parse_jwt_header(bytes: &[u8]) -> JsonResult<()> {
    let mut cursor = JsonCursor::new(bytes, false);
    let mut alg = false;
    cursor.object(0, |cursor, key| {
        if key == "alg" {
            if alg || cursor.string()? != "HS256" {
                return Err(ParseError);
            }
            alg = true;
        } else {
            cursor.skip_value(1)?;
        }
        Ok(())
    })?;
    cursor.finish()?;
    if alg { Ok(()) } else { Err(ParseError) }
}

pub fn parse_jwt_claims(bytes: &[u8]) -> JsonResult<Claims<'_>> {
    let mut cursor = JsonCursor::new(bytes, false);
    let mut claims = Claims {
        sub: None,
        username: None,
        exp: None,
        nbf: None,
    };
    let mut seen = 0u8;
    cursor.object(0, |cursor, key| {
        let bit = match key.as_ref() {
            "sub" => 1,
            "username" => 2,
            "exp" => 4,
            "nbf" => 8,
            _ => 0,
        };
        if seen & bit != 0 {
            return Err(ParseError);
        }
        seen |= bit;
        match bit {
            1 => claims.sub = cursor.text_or_skip()?,
            2 => claims.username = cursor.text_or_skip()?,
            4 => claims.exp = cursor.optional_number()?,
            8 => claims.nbf = cursor.optional_number()?,
            _ => cursor.skip_value(1)?,
        }
        Ok(())
    })?;
    cursor.finish()?;
    Ok(claims)
}

pub fn parse_post_body(bytes: &[u8]) -> JsonResult<Option<Cow<'_, str>>> {
    let mut cursor = JsonCursor::new(bytes, true);
    cursor.skip_ws();
    let mut body = None;
    if cursor.bytes.get(cursor.index) == Some(&b'{') {
        cursor.object(0, |cursor, key| {
            if key == "body" {
                body = cursor.text_or_skip()?;
            } else {
                cursor.skip_value(1)?;
            }
            Ok(())
        })?;
    } else {
        cursor.skip_value(0)?;
    }
    cursor.finish()?;
    Ok(body)
}

#[inline]
pub fn trim_js_str(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_whitespace() || c == '\u{feff}')
}

#[inline]
pub fn utf16_len_le_500(s: &str) -> bool {
    if s.len() <= 500 {
        return true;
    }
    let mut count = 0usize;
    for _ in s.chars() {
        count += 1;
        if count > 500 {
            return false;
        }
    }
    true
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Get,
    Post,
    Other,
}

pub struct HttpFrame<'a> {
    pub method: Method,
    pub path: &'a [u8],
    pub authorization: Option<&'a [u8]>,
    pub body: Cow<'a, [u8]>,
    pub consumed: usize,
    pub close: bool,
}

pub enum FrameStatus<'a> {
    Incomplete { expect_continue: bool },
    Complete(HttpFrame<'a>),
}

#[inline]
fn find_crlf(buf: &[u8]) -> Option<usize> {
    let len = buf.len();
    if len < 2 {
        return None;
    }
    let mut i = 0;
    #[cfg(target_arch = "x86_64")]
    while i + 16 <= len {
        let mask = unsafe {
            use std::arch::x86_64::*;
            let v = _mm_loadu_si128(buf.as_ptr().add(i).cast());
            _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(b'\r' as i8))) as u32
        };
        if mask != 0 {
            let mut m = mask;
            while m != 0 {
                let bit = m.trailing_zeros() as usize;
                let pos = i + bit;
                if pos + 1 < len && buf[pos + 1] == b'\n' {
                    return Some(pos);
                }
                m &= m - 1;
            }
        }
        i += 16;
    }
    while i + 1 < len {
        if buf[i] == b'\r' && buf[i + 1] == b'\n' {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[inline]
fn trim_ows(mut s: &[u8]) -> &[u8] {
    while let Some(&first) = s.first() {
        if first == b' ' || first == b'\t' {
            s = &s[1..];
        } else {
            break;
        }
    }
    while let Some(&last) = s.last() {
        if last == b' ' || last == b'\t' {
            s = &s[..s.len() - 1];
        } else {
            break;
        }
    }
    s
}

pub fn parse_http_frame(input: &[u8]) -> Result<FrameStatus<'_>, ()> {
    // Fast scan for end of headers (\r\n\r\n)
    let Some(first_crlf) = find_crlf(input) else {
        return if input.len() > 65536 {
            Err(())
        } else {
            Ok(FrameStatus::Incomplete {
                expect_continue: false,
            })
        };
    };

    let req_line = &input[..first_crlf];
    let sp1 = req_line.iter().position(|&b| b == b' ').ok_or(())?;
    if sp1 == 0 {
        return Err(());
    }
    let method_bytes = &req_line[..sp1];
    let rest_line = &req_line[sp1 + 1..];
    let sp2 = rest_line.iter().position(|&b| b == b' ').ok_or(())?;
    if sp2 == 0 {
        return Err(());
    }
    let raw_path = &rest_line[..sp2];
    if !raw_path.starts_with(b"/") {
        return Err(());
    }
    let version = &rest_line[sp2 + 1..];
    let is_http11 = if version == b"HTTP/1.1" {
        true
    } else if version == b"HTTP/1.0" {
        false
    } else {
        return Err(());
    };

    let method = match method_bytes {
        b"GET" => Method::Get,
        b"POST" => Method::Post,
        _ => {
            if !method_bytes
                .iter()
                .all(|&c| c.is_ascii_alphabetic() || c == b'-' || c == b'_')
            {
                return Err(());
            }
            Method::Other
        }
    };

    let mut pos = first_crlf + 2;
    let mut authorization: Option<&[u8]> = None;
    let mut content_length: Option<usize> = None;
    let mut chunked = false;
    let mut expect_continue = false;
    let mut close = !is_http11;
    let mut header_count = 0usize;

    loop {
        let Some(rel_crlf) = find_crlf(&input[pos..]) else {
            return if input.len() > 65536 {
                Err(())
            } else {
                Ok(FrameStatus::Incomplete {
                    expect_continue: false,
                })
            };
        };
        if rel_crlf == 0 {
            pos += 2;
            break;
        }
        if pos + rel_crlf > 65536 || header_count >= 64 {
            return Err(());
        }
        header_count += 1;
        let line = &input[pos..pos + rel_crlf];
        pos += rel_crlf + 2;

        let colon = line.iter().position(|&b| b == b':').ok_or(())?;
        if colon == 0 {
            return Err(());
        }
        let name = &line[..colon];
        if name.iter().any(|&b| b <= b' ' || b == 127) {
            return Err(());
        }
        let val = trim_ows(&line[colon + 1..]);

        match name.len() {
            13 if name.eq_ignore_ascii_case(b"authorization") => {
                if authorization.is_some() {
                    return Err(());
                }
                authorization = Some(val);
            }
            14 if name.eq_ignore_ascii_case(b"content-length") => {
                if content_length.is_some() {
                    return Err(());
                }
                if val.is_empty() || val.len() > 8 || !val.iter().all(u8::is_ascii_digit) {
                    return Err(());
                }
                let mut n = 0usize;
                for &d in val {
                    n = n * 10 + (d - b'0') as usize;
                }
                if n > 1_048_576 {
                    return Err(());
                }
                content_length = Some(n);
            }
            10 if name.eq_ignore_ascii_case(b"connection") => {
                for tok in val.split(|&c| c == b',') {
                    let t = trim_ows(tok);
                    if t.eq_ignore_ascii_case(b"close") {
                        close = true;
                    } else if t.eq_ignore_ascii_case(b"keep-alive") && !is_http11 {
                        close = false;
                    }
                }
            }
            17 if name.eq_ignore_ascii_case(b"transfer-encoding") => {
                if chunked || !val.eq_ignore_ascii_case(b"chunked") {
                    return Err(());
                }
                chunked = true;
            }
            6 if name.eq_ignore_ascii_case(b"expect") => {
                if !val.eq_ignore_ascii_case(b"100-continue") {
                    return Err(());
                }
                expect_continue = is_http11;
            }
            _ => {}
        }
    }

    let path = match raw_path.iter().position(|&b| b == b'?') {
        Some(q) => &raw_path[..q],
        None => raw_path,
    };

    let (body, consumed) = if chunked {
        if content_length.is_some() {
            return Err(());
        }
        let Some((decoded, n)) = decode_chunks(&input[pos..])? else {
            return Ok(FrameStatus::Incomplete { expect_continue });
        };
        (Cow::Owned(decoded), pos + n)
    } else {
        let len = content_length.unwrap_or(0);
        if input.len() < pos + len {
            return Ok(FrameStatus::Incomplete { expect_continue });
        }
        (Cow::Borrowed(&input[pos..pos + len]), pos + len)
    };

    Ok(FrameStatus::Complete(HttpFrame {
        method,
        path,
        authorization,
        body,
        consumed,
        close,
    }))
}

#[cold]
fn decode_chunks(input: &[u8]) -> Result<Option<(Vec<u8>, usize)>, ()> {
    let mut i = 0;
    let mut output = Vec::new();
    loop {
        let Some(n) = find_crlf(&input[i..]) else {
            return Ok(None);
        };
        let size_part = input[i..i + n].split(|&c| c == b';').next().ok_or(())?;
        if size_part.is_empty()
            || size_part.len() > 8
            || !size_part.iter().all(u8::is_ascii_hexdigit)
        {
            return Err(());
        }
        let size_str = unsafe { std::str::from_utf8_unchecked(size_part) };
        let size = usize::from_str_radix(size_str, 16).map_err(|_| ())?;
        i += n + 2;
        if size == 0 {
            loop {
                let Some(n) = find_crlf(&input[i..]) else {
                    return Ok(None);
                };
                i += n + 2;
                if n == 0 {
                    return Ok(Some((output, i)));
                }
                if i > 65536 + output.len() {
                    return Err(());
                }
            }
        }
        if size > 1_048_576 - output.len() {
            return Err(());
        }
        if input.len() < i + size + 2 {
            return Ok(None);
        }
        if &input[i + size..i + size + 2] != b"\r\n" {
            return Err(());
        }
        output.extend_from_slice(&input[i..i + size]);
        i += size + 2;
    }
}

#[inline]
pub fn write_usize_ascii(mut n: usize, buf: &mut [u8; 20]) -> &[u8] {
    if n == 0 {
        buf[19] = b'0';
        return &buf[19..20];
    }
    let mut i = 20;
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    &buf[i..20]
}
