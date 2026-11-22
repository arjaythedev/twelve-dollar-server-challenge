//! JWT HS256 verification, per SPEC.md:
//!
//! - `Authorization: Bearer <jwt>` ;  missing header or wrong prefix →
//!   `missing bearer token` (401)
//! - bad signature, expired, not HS256, malformed → `invalid or expired token`
//! - valid signature but `sub` not a positive-integer string or `username`
//!   not a string → `invalid token payload`
//!
//! Verified on every request (rule 5 forbids remembering verdicts). The
//! HMAC key schedule for the fixed secret is a compile-time constant, not
//! cached request data.
//!
//! Perf note (optimization pass): with a fixed key, HMAC's inner/outer
//! SHA-256 midstates can be precomputed once (2 block compressions saved
//! per request). Kept simple+correct now; swap to midstate reuse with the
//! io_uring pass work.

use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine as _;
use hmac::{Hmac, Mac};
use sha2::Sha256;

type HmacSha256 = Hmac<Sha256>;

/// Verification outcome; each variant maps 1:1 to a SPEC error row.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JwtError {
    /// 401 `missing bearer token`
    MissingBearer,
    /// 401 `invalid or expired token`
    InvalidToken,
    /// 401 `invalid token payload`
    InvalidPayload,
}

/// What the server needs per request, decoded and checked.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Claims {
    pub user_id: i64,
    /// `username` bytes, used as-is (SPEC: no lookup).
    pub username: Vec<u8>,
}

/// Base64url decode, unpadded or padded tolerated, garbage rejected.
/// Padding is stripped before `URL_SAFE_NO_PAD` decoding (JWTs omit it;
/// some generators keep it). Allocation is bounded by token length.
pub fn base64url_decode(input: &[u8]) -> Option<Vec<u8>> {
    let end = input.iter().rposition(|&b| b != b'=').map_or(0, |p| p + 1);
    let cleaned = &input[..end];
    let mut out = vec![0u8; cleaned.len() / 4 * 3 + 3];
    let written = URL_SAFE_NO_PAD.decode_slice(cleaned, &mut out).ok()?;
    out.truncate(written);
    Some(out)
}

/// Constant-time equality over byte slices of equal length.
fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// HMAC-SHA256 over `data` with `key` (RFC 2104, via the estate-standard
/// `hmac` + `sha2` crates; lineage: oauth-toolkit/src/crypto.rs uses the same).
pub fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut mac = <HmacSha256 as Mac>::new_from_slice(key)
        .expect("HMAC accepts every key length per RFC 2104; startup-validated secret");
    mac.update(data);
    mac.finalize().into_bytes().into()
}

/// Startup-time key schedule: a clonable MAC whose inner state is already
/// keyed, so per-request cost is clone + update + finalize (P3 lever: the
/// key schedule compression is paid once, not per request).
#[derive(Clone)]
pub struct KeyedMac(HmacSha256);

impl KeyedMac {
    /// `secret` is the fixed `JWT_SECRET` validated at startup.
    pub fn new(secret: &[u8]) -> Self {
        let mac = <HmacSha256 as Mac>::new_from_slice(secret)
            .expect("HMAC accepts every key length; startup constant");
        KeyedMac(mac)
    }

    pub fn sign(&self, data: &[u8]) -> [u8; 32] {
        let mut mac = self.0.clone();
        mac.update(data);
        mac.finalize().into_bytes().into()
    }

    /// Constant-time verify.
    pub fn verify(&self, data: &[u8], sig: &[u8]) -> bool {
        ct_eq(&self.sign(data), sig)
    }
}

/// Extract a string value for `key` from a small JSON object (flat, SPEC
/// payload shape). Returns the byte view of the value (inside quotes for
/// strings; raw token bytes for numbers ;  caller disambiguates).
/// Totality: arbitrary bytes → `None` or a valid view, never panic.
fn json_member<'a>(obj: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let mut i = 0;
    while i < obj.len() {
        if obj[i] != b'"' {
            i += 1;
            continue;
        }
        // key string
        let start = i + 1;
        let mut j = start;
        while j < obj.len() && obj[j] != b'"' {
            if obj[j] == b'\\' {
                j += 1; // skip escaped char
            }
            j += 1;
        }
        if j >= obj.len() {
            return None;
        }
        let found_key = &obj[start..j];
        i = j + 1;
        // colon
        while i < obj.len() && (obj[i] == b' ' || obj[i] == b':') {
            i += 1;
        }
        if i >= obj.len() || obj[i - 1] != b':' {
            // skip to value start even without spaces
            while i < obj.len() && obj[i] == b':' {
                i += 1;
            }
        }
        if i >= obj.len() {
            return None;
        }
        if found_key == key {
            // value: string or number token
            if obj[i] == b'"' {
                let vs = i + 1;
                let mut k = vs;
                while k < obj.len() && obj[k] != b'"' {
                    if obj[k] == b'\\' {
                        k += 1;
                    }
                    k += 1;
                }
                return if k < obj.len() { Some(&obj[vs..k]) } else { None };
            }
            // number: scan to , or }
            let vs = i;
            let mut k = i;
            while k < obj.len() && obj[k] != b',' && obj[k] != b'}' {
                k += 1;
            }
            return Some(&obj[vs..k]);
        }
        // not our key: skip the value (string or number)
        if obj[i] == b'"' {
            let mut k = i + 1;
            while k < obj.len() && obj[k] != b'"' {
                if obj[k] == b'\\' {
                    k += 1;
                }
                k += 1;
            }
            i = k + 1;
        } else {
            while i < obj.len() && obj[i] != b',' && obj[i] != b'}' {
                i += 1;
            }
        }
    }
    None
}

/// Verify `authorization_value` (raw header value after trimming) with the
/// startup-keyed MAC (P3: the HMAC key schedule is paid once). Returns the
/// claims or the SPEC error row.
pub fn verify(
    authorization_value: Option<&[u8]>,
    mac: &KeyedMac,
    now: i64,
) -> Result<Claims, JwtError> {
    let value = match authorization_value {
        Some(v) if v.starts_with(b"Bearer ") => &v[7..],
        _ => return Err(JwtError::MissingBearer),
    };
    // token = header.payload.signature (three dot-separated b64url parts)
    let mut parts = value.split(|&b| b == b'.');
    let header_b64 = parts.next().unwrap_or(&[]);
    let payload_b64 = parts.next().unwrap_or(&[]);
    let sig_b64 = parts.next().unwrap_or(&[]);
    if parts.next().is_some() || header_b64.is_empty() || payload_b64.is_empty() || sig_b64.is_empty()
    {
        return Err(JwtError::InvalidToken);
    }
    let sig = base64url_decode(sig_b64).ok_or(JwtError::InvalidToken)?;
    if sig.len() != 32 {
        return Err(JwtError::InvalidToken);
    }
    // signing input = header.payload (original bytes)
    let dot = value.len() - sig_b64.len() - 1;
    let signing_input = &value[..dot];
    if !mac.verify(signing_input, &sig) {
        return Err(JwtError::InvalidToken);
    }
    // header: must declare HS256
    let header = base64url_decode(header_b64).ok_or(JwtError::InvalidToken)?;
    let alg = json_member(&header, b"alg").ok_or(JwtError::InvalidToken)?;
    if alg != b"HS256" {
        return Err(JwtError::InvalidToken);
    }
    // payload: exp + claims
    let payload = base64url_decode(payload_b64).ok_or(JwtError::InvalidToken)?;
    let exp = json_member(&payload, b"exp").ok_or(JwtError::InvalidToken)?;
    let exp: i64 = std::str::from_utf8(exp)
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .ok_or(JwtError::InvalidToken)?;
    if now >= exp {
        return Err(JwtError::InvalidToken);
    }
    let sub = json_member(&payload, b"sub").ok_or(JwtError::InvalidPayload)?;
    let user_id: i64 = std::str::from_utf8(sub)
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .ok_or(JwtError::InvalidPayload)?;
    if user_id <= 0 {
        return Err(JwtError::InvalidPayload);
    }
    let username = json_member(&payload, b"username").ok_or(JwtError::InvalidPayload)?;
    if username.is_empty() {
        return Err(JwtError::InvalidPayload);
    }
    Ok(Claims { user_id, username: username.to_vec() })
}

#[cfg(test)]
mod tests {
    use super::*;

    // Minimal deterministic HS256 token builder for tests (mirrors seed tokens).
    fn make_token(header: &str, payload: &str, secret: &[u8]) -> String {
        let b64 = |b: &[u8]| -> String {
            const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            let mut s = String::new();
            for c in b.chunks(3) {
                let n = ((c[0] as u32) << 16) | ((*c.get(1).unwrap_or(&0) as u32) << 8)
                    | (*c.get(2).unwrap_or(&0) as u32);
                s.push(T[(n >> 18) as usize & 63] as char);
                s.push(T[(n >> 12) as usize & 63] as char);
                if c.len() > 1 {
                    s.push(T[(n >> 6) as usize & 63] as char);
                }
                if c.len() > 2 {
                    s.push(T[n as usize & 63] as char);
                }
            }
            s
        };
        let h = b64(header.as_bytes());
        let p = b64(payload.as_bytes());
        let sig = hmac_sha256(secret, format!("{h}.{p}").as_bytes());
        format!("{h}.{p}.{}", b64(&sig))
    }

    const SECRET: &[u8] = b"twelve-dollar-challenge";

    fn valid_payload(exp: i64) -> String {
        r#"{"sub":"123","username":"wyatt","iat":1700000000,"exp":"#.to_owned() + &exp.to_string() + "}"
    }

    #[test]
    fn valid_token_verifies() {
        let mac = test_mac();
        let t = make_token(r#"{"alg":"HS256","typ":"JWT"}"#, &valid_payload(9_999_999_999), SECRET);
        let c = verify(Some(format!("Bearer {t}").as_bytes()), &mac, 1_700_000_100).unwrap();
        assert_eq!(c.user_id, 123);
        assert_eq!(c.username, b"wyatt".to_vec());
    }

    fn test_mac() -> KeyedMac { KeyedMac::new(SECRET) }

    #[test]
    fn missing_or_wrong_prefix() {
        let mac = test_mac();
        assert_eq!(verify(None, &mac, 0), Err(JwtError::MissingBearer));
        assert_eq!(verify(Some(b"Basic abc"), &mac, 0), Err(JwtError::MissingBearer));
        assert_eq!(verify(Some(b"Bearer"), &mac, 0), Err(JwtError::MissingBearer));
    }

    #[test]
    fn tampered_payload_fails_signature() {
        let mac = test_mac();
        let t = make_token(r#"{"alg":"HS256"}"#, &valid_payload(9_999_999_999), SECRET);
        let mut t2 = format!("Bearer {t}").into_bytes();
        // flip a payload char
        let mid = t2.len() / 2;
        t2[mid] = if t2[mid] == b'a' { b'b' } else { b'a' };
        assert_eq!(verify(Some(&t2), &mac, 0), Err(JwtError::InvalidToken));
    }

    #[test]
    fn wrong_alg_rejected() {
        let mac = test_mac();
        let t = make_token(r#"{"alg":"HS512"}"#, &valid_payload(9_999_999_999), SECRET);
        assert_eq!(verify(Some(format!("Bearer {t}").as_bytes()), &mac, 0), Err(JwtError::InvalidToken));
    }

    #[test]
    fn expired_rejected() {
        let mac = test_mac();
        let t = make_token(r#"{"alg":"HS256"}"#, &valid_payload(1_700_000_000), SECRET);
        assert_eq!(verify(Some(format!("Bearer {t}").as_bytes()), &mac, 1_700_000_000), Err(JwtError::InvalidToken));
    }

    #[test]
    fn bad_sub_or_username_payload_row() {
        let mac = test_mac();
        let t = make_token(r#"{"alg":"HS256"}"#, r#"{"sub":"abc","username":"w","exp":9999999999}"#, SECRET);
        assert_eq!(verify(Some(format!("Bearer {t}").as_bytes()), &mac, 0), Err(JwtError::InvalidPayload));
        let t = make_token(r#"{"alg":"HS256"}"#, r#"{"sub":"0","username":"w","exp":9999999999}"#, SECRET);
        assert_eq!(verify(Some(format!("Bearer {t}").as_bytes()), &mac, 0), Err(JwtError::InvalidPayload));
        let t = make_token(r#"{"alg":"HS256"}"#, r#"{"sub":"5","exp":9999999999}"#, SECRET);
        assert_eq!(verify(Some(format!("Bearer {t}").as_bytes()), &mac, 0), Err(JwtError::InvalidPayload));
    }

    #[test]
    fn malformed_tokens_invalid_row() {
        let mac = test_mac();
        assert_eq!(verify(Some(b"Bearer a.b"), &mac, 0), Err(JwtError::InvalidToken));
        assert_eq!(verify(Some(b"Bearer !.x.y"), &mac, 0), Err(JwtError::InvalidToken));
        assert_eq!(verify(Some(b"Bearer a.b.c.d"), &mac, 0), Err(JwtError::InvalidToken));
        assert_eq!(verify(Some(b"Bearer AA.AA.AA"), &mac, 0), Err(JwtError::InvalidToken)); // sig len != 32
    }

    #[test]
    fn base64url_known_vectors() {
        // RFC 4648 test vectors, URL-safe alphabet, no padding.
        assert_eq!(base64url_decode(b""), Some(vec![]));
        assert_eq!(base64url_decode(b"Zm9v"), Some(b"foo".to_vec()));
        assert_eq!(base64url_decode(b"Zm9vYg"), Some(b"foob".to_vec()));
        assert_eq!(base64url_decode(b"Zm9vYmE"), Some(b"fooba".to_vec()));
        assert_eq!(base64url_decode(b"Zm9vYmFy"), Some(b"foobar".to_vec()));
        assert_eq!(base64url_decode(b"Zm9vYmFy=="), Some(b"foobar".to_vec())); // padding tolerated
        assert_eq!(base64url_decode(b"A"), None); // 1 leftover char
        assert_eq!(base64url_decode(b"++--"), None); // standard-alphabet chars rejected
    }
}
