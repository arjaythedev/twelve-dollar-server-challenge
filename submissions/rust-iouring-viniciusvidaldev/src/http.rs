#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Get,
    Post,
    Put,
    Delete,
}

impl Method {
    fn parse(bytes: &[u8]) -> Option<Self> {
        match bytes {
            b"GET" => Some(Method::Get),
            b"POST" => Some(Method::Post),
            b"PUT" => Some(Method::Put),
            b"DELETE" => Some(Method::Delete),
            _ => None,
        }
    }
}

pub struct Request<'a> {
    pub method: Method,
    pub path: &'a [u8],
    pub authorization: Option<&'a [u8]>,
    pub body: &'a [u8],
    pub len: usize,
}

pub enum Parse<'a> {
    Done(Request<'a>),
    Partial,
    Invalid,
}

const MAX_HEAD: usize = 8192;

pub fn parse(input: &[u8]) -> Parse<'_> {
    let Some(head_end) = input.windows(4).position(|w| w == b"\r\n\r\n") else {
        if input.len() > MAX_HEAD {
            return Parse::Invalid;
        }
        return Parse::Partial;
    };

    let head = &input[..head_end];

    let mut lines = head.split(|&b| b == b'\n').map(<[u8]>::trim_ascii_end);
    let Some(request_line) = lines.next() else {
        return Parse::Invalid;
    };
    let mut parts = request_line.splitn(3, |&b| b == b' ');
    let Some(method) = parts.next().and_then(Method::parse) else {
        return Parse::Invalid;
    };
    let Some(uri) = parts.next() else {
        return Parse::Invalid;
    };
    let Some(b"HTTP/1.1") = parts.next() else {
        return Parse::Invalid;
    };

    let mut content_length = 0;
    let mut authorization = None;
    for line in lines {
        let Some(colon) = line.iter().position(|&b| b == b':') else {
            return Parse::Invalid;
        };
        let name = &line[..colon];
        let value = line[colon + 1..].trim_ascii();
        if name.eq_ignore_ascii_case(b"content-length") {
            let Some(n) = parse_usize(value) else {
                return Parse::Invalid;
            };
            content_length = n;
        } else if name.eq_ignore_ascii_case(b"authorization") {
            authorization = Some(value);
        }
    }
    let body_start = head_end + 4;
    let len = body_start + content_length;
    if input.len() < len {
        return Parse::Partial;
    }

    Parse::Done(Request {
        method,
        path: uri,
        authorization,
        body: &input[body_start..len],
        len,
    })
}

fn parse_usize(b: &[u8]) -> Option<usize> {
    if b.is_empty() {
        return None;
    }
    let mut n: usize = 0;
    for &d in b {
        if !d.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add((d - b'0') as usize)?;
    }
    Some(n)
}
