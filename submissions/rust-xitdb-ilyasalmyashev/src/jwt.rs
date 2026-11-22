//! HS256 JWT verification. Every request's signature is checked (rule 5); only the HMAC key's
//! padded blocks are hashed once at startup.
use crate::json::{self, Val};
use sha2::digest::{FixedOutput, Update};
use sha2::Sha256;

pub enum AuthError {
    Missing,
    Invalid,
    Payload,
}

pub struct Claims {
    pub user_id: i64,
    /// Unescaped `username`.
    pub username: Vec<u8>,
}

pub struct Verifier {
    inner: Sha256,
    outer: Sha256,
    scratch: Vec<u8>,
}

const fn b64_table() -> [u8; 256] {
    let mut t = [0xffu8; 256];
    let a = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut i = 0;
    while i < 64 {
        t[a[i] as usize] = i as u8;
        i += 1;
    }
    t
}
static B64: [u8; 256] = b64_table();

/// Unpadded base64url. Appends to `out`; Err on any invalid character or length.
fn b64url(s: &[u8], out: &mut Vec<u8>) -> Result<(), ()> {
    if s.len() % 4 == 1 {
        return Err(());
    }
    let mut chunks = s.chunks_exact(4);
    for c in &mut chunks {
        let (a, b, d, e) = (B64[c[0] as usize], B64[c[1] as usize], B64[c[2] as usize], B64[c[3] as usize]);
        if (a | b | d | e) & 0xc0 != 0 {
            return Err(());
        }
        let v = (a as u32) << 18 | (b as u32) << 12 | (d as u32) << 6 | e as u32;
        out.extend_from_slice(&[(v >> 16) as u8, (v >> 8) as u8, v as u8]);
    }
    let r = chunks.remainder();
    if !r.is_empty() {
        let mut v = 0u32;
        for (i, &ch) in r.iter().enumerate() {
            let x = B64[ch as usize];
            if x & 0xc0 != 0 {
                return Err(());
            }
            v |= (x as u32) << (18 - 6 * i);
        }
        // Canonical encoding only: the unused low bits of the last character must be zero,
        // so exactly one spelling of a signature verifies.
        let unused = if r.len() == 2 { v & 0xffff } else { v & 0xff };
        if r.len() < 2 || unused != 0 {
            return Err(());
        }
        out.push((v >> 16) as u8);
        if r.len() == 3 {
            out.push((v >> 8) as u8);
        }
    }
    Ok(())
}

impl Verifier {
    pub fn new(secret: &[u8]) -> Self {
        let mut key = [0u8; 64];
        if secret.len() > 64 {
            let d = Sha256::default().chain(secret).finalize_fixed();
            key[..32].copy_from_slice(&d);
        } else {
            key[..secret.len()].copy_from_slice(secret);
        }
        let ipad: Vec<u8> = key.iter().map(|b| b ^ 0x36).collect();
        let opad: Vec<u8> = key.iter().map(|b| b ^ 0x5c).collect();
        Verifier {
            inner: Sha256::default().chain(&ipad),
            outer: Sha256::default().chain(&opad),
            scratch: Vec::with_capacity(512),
        }
    }

    /// `header` is the Authorization header value, if any. `now` is Unix seconds.
    pub fn verify(&mut self, header: Option<&[u8]>, now: f64) -> Result<Claims, AuthError> {
        let token = match header {
            Some(h) if h.starts_with(b"Bearer ") => &h[7..],
            _ => return Err(AuthError::Missing),
        };
        let mut parts = token.splitn(4, |&b| b == b'.');
        let (h, p, s) = match (parts.next(), parts.next(), parts.next(), parts.next()) {
            (Some(h), Some(p), Some(s), None) => (h, p, s),
            _ => return Err(AuthError::Invalid),
        };
        let buf = &mut self.scratch;
        buf.clear();
        b64url(s, buf).map_err(|_| AuthError::Invalid)?;
        if buf.len() != 32 {
            return Err(AuthError::Invalid);
        }
        let signed = &token[..h.len() + 1 + p.len()];
        let want = {
            let ih = self.inner.clone().chain(signed).finalize_fixed();
            self.outer.clone().chain(&ih).finalize_fixed()
        };
        let mut diff = 0u8;
        for (a, b) in want.iter().zip(buf.iter()) {
            diff |= a ^ b;
        }
        if diff != 0 {
            return Err(AuthError::Invalid);
        }
        // Header: must say HS256.
        buf.clear();
        b64url(h, buf).map_err(|_| AuthError::Invalid)?;
        let mut alg_ok = false;
        let is_obj = json::parse_object(buf, &mut |k, v| {
            if k == b"alg" {
                alg_ok = match v {
                    Val::Str(s, false) => s == b"HS256",
                    Val::Str(s, true) => {
                        let mut t = Vec::new();
                        json::unescape(s, &mut t);
                        t == b"HS256"
                    }
                    _ => false,
                };
            }
        })
        .map_err(|_| AuthError::Invalid)?;
        if !is_obj || !alg_ok {
            return Err(AuthError::Invalid);
        }
        // Payload.
        buf.clear();
        b64url(p, buf).map_err(|_| AuthError::Invalid)?;
        let mut exp: Option<f64> = None;
        let mut sub: Option<(&[u8], bool)> = None;
        let mut name: Option<(&[u8], bool)> = None;
        let is_obj = json::parse_object(buf, &mut |k, v| match k {
            b"exp" => exp = if let Val::Num(n) = v { Some(n) } else { None },
            b"sub" => sub = if let Val::Str(s, e) = v { Some((s, e)) } else { None },
            b"username" => name = if let Val::Str(s, e) = v { Some((s, e)) } else { None },
            _ => {}
        })
        .map_err(|_| AuthError::Invalid)?;
        if !is_obj {
            return Err(AuthError::Invalid);
        }
        match exp {
            Some(e) if now < e => {}
            _ => return Err(AuthError::Invalid),
        }
        let user_id = match sub {
            Some((s, false)) => parse_positive(s).ok_or(AuthError::Payload)?,
            Some((s, true)) => {
                let mut t = Vec::new();
                json::unescape(s, &mut t);
                parse_positive(&t).ok_or(AuthError::Payload)?
            }
            None => return Err(AuthError::Payload),
        };
        let username = match name {
            Some((s, false)) => s.to_vec(),
            Some((s, true)) => {
                let mut t = Vec::new();
                json::unescape(s, &mut t);
                t
            }
            None => return Err(AuthError::Payload),
        };
        Ok(Claims { user_id, username })
    }
}

/// A positive decimal integer with no sign, spaces or leading zeros that fits in an i64.
pub fn parse_positive(s: &[u8]) -> Option<i64> {
    if s.is_empty() || s.len() > 18 || s[0] == b'0' || !s.iter().all(u8::is_ascii_digit) {
        return None;
    }
    Some(s.iter().fold(0i64, |a, &c| a * 10 + (c - b'0') as i64))
}
