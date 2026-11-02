use crate::http::{parse_jwt_claims, parse_jwt_header};
use std::borrow::Cow;

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const H0: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

#[inline(always)]
fn compress_scalar(state: &mut [u32; 8], block: &[u8; 64]) {
    let mut w = [0u32; 64];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([
            block[i * 4],
            block[i * 4 + 1],
            block[i * 4 + 2],
            block[i * 4 + 3],
        ]);
    }
    for i in 16..64 {
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16]
            .wrapping_add(s0)
            .wrapping_add(w[i - 7])
            .wrapping_add(s1);
    }

    let mut a = state[0];
    let mut b = state[1];
    let mut c = state[2];
    let mut d = state[3];
    let mut e = state[4];
    let mut f = state[5];
    let mut g = state[6];
    let mut h = state[7];

    for i in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = (e & f) ^ ((!e) & g);
        let temp1 = h
            .wrapping_add(s1)
            .wrapping_add(ch)
            .wrapping_add(K[i])
            .wrapping_add(w[i]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let temp2 = s0.wrapping_add(maj);

        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(temp1);
        d = c;
        c = b;
        b = a;
        a = temp1.wrapping_add(temp2);
    }

    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
    state[4] = state[4].wrapping_add(e);
    state[5] = state[5].wrapping_add(f);
    state[6] = state[6].wrapping_add(g);
    state[7] = state[7].wrapping_add(h);
}

#[cfg(target_arch = "x86_64")]
#[allow(unused_assignments)]
#[target_feature(enable = "sha,sse4.1,ssse3")]
unsafe fn compress_shani(state: &mut [u32; 8], block: &[u8; 64]) {
    use std::arch::x86_64::*;

    let mask = _mm_set_epi64x(0x0c0d0e0f08090a0b, 0x0405060700010203);

    let mut abef = _mm_loadu_si128(state.as_ptr() as *const __m128i);
    let mut cdgh = _mm_loadu_si128(state.as_ptr().add(4) as *const __m128i);

    let tmp = _mm_shuffle_epi32(abef, 0xB1);
    let cdgh_swap = _mm_shuffle_epi32(cdgh, 0x1B);
    abef = _mm_alignr_epi8(tmp, cdgh_swap, 8);
    cdgh = _mm_blend_epi16(cdgh_swap, tmp, 0xF0);

    let abef_save = abef;
    let cdgh_save = cdgh;

    let ptr = block.as_ptr() as *const __m128i;
    let mut msg0 = _mm_shuffle_epi8(_mm_loadu_si128(ptr), mask);
    let mut msg1 = _mm_shuffle_epi8(_mm_loadu_si128(ptr.add(1)), mask);
    let mut msg2 = _mm_shuffle_epi8(_mm_loadu_si128(ptr.add(2)), mask);
    let mut msg3 = _mm_shuffle_epi8(_mm_loadu_si128(ptr.add(3)), mask);

    macro_rules! rnd4 {
        ($m_cur:ident, $m_prev:ident, $m_prev2:ident, $m_next:ident, $k_idx:expr, $do_msg1:expr, $do_msg2:expr) => {{
            let k = _mm_loadu_si128(K.as_ptr().add($k_idx) as *const __m128i);
            let mut msg = _mm_add_epi32($m_cur, k);
            cdgh = _mm_sha256rnds2_epu32(cdgh, abef, msg);
            if $do_msg2 {
                let t = _mm_alignr_epi8($m_cur, $m_prev, 4);
                $m_next = _mm_add_epi32($m_next, t);
                $m_next = _mm_sha256msg2_epu32($m_next, $m_cur);
            }
            msg = _mm_shuffle_epi32(msg, 0x0E);
            abef = _mm_sha256rnds2_epu32(abef, cdgh, msg);
            if $do_msg1 {
                $m_prev2 = _mm_sha256msg1_epu32($m_prev2, $m_prev);
            }
        }};
    }

    rnd4!(msg0, msg3, msg3, msg0, 0, false, false);
    rnd4!(msg1, msg0, msg0, msg1, 4, true, false);
    rnd4!(msg2, msg1, msg1, msg0, 8, true, true);
    rnd4!(msg3, msg2, msg2, msg1, 12, true, true);

    rnd4!(msg0, msg3, msg3, msg2, 16, true, true);
    rnd4!(msg1, msg0, msg0, msg3, 20, true, true);
    rnd4!(msg2, msg1, msg1, msg0, 24, true, true);
    rnd4!(msg3, msg2, msg2, msg1, 28, true, true);

    rnd4!(msg0, msg3, msg3, msg2, 32, true, true);
    rnd4!(msg1, msg0, msg0, msg3, 36, true, true);
    rnd4!(msg2, msg1, msg1, msg0, 40, true, true);
    rnd4!(msg3, msg2, msg2, msg1, 44, true, true);

    rnd4!(msg0, msg3, msg3, msg2, 48, true, true);
    rnd4!(msg1, msg0, msg0, msg3, 52, false, true);
    rnd4!(msg2, msg1, msg1, msg0, 56, false, false);
    rnd4!(msg3, msg2, msg2, msg1, 60, false, false);

    abef = _mm_add_epi32(abef, abef_save);
    cdgh = _mm_add_epi32(cdgh, cdgh_save);

    let tmp2 = _mm_shuffle_epi32(abef, 0x1B);
    let cdgh2 = _mm_shuffle_epi32(cdgh, 0xB1);
    let s0 = _mm_blend_epi16(tmp2, cdgh2, 0xF0);
    let s1 = _mm_alignr_epi8(cdgh2, tmp2, 8);

    _mm_storeu_si128(state.as_mut_ptr() as *mut __m128i, s0);
    _mm_storeu_si128(state.as_mut_ptr().add(4) as *mut __m128i, s1);
}

#[inline(always)]
fn compress(state: &mut [u32; 8], block: &[u8; 64], use_shani: bool) {
    #[cfg(target_arch = "x86_64")]
    if use_shani {
        unsafe {
            compress_shani(state, block);
        }
        return;
    }
    let _ = use_shani;
    compress_scalar(state, block);
}

fn sha256_oneshot(data: &[u8], use_shani: bool) -> [u8; 32] {
    let mut state = H0;
    let mut chunks = data.chunks_exact(64);
    for chunk in &mut chunks {
        let block: &[u8; 64] = chunk.try_into().unwrap();
        compress(&mut state, block, use_shani);
    }
    let rem = chunks.remainder();
    let mut block = [0u8; 64];
    block[..rem.len()].copy_from_slice(rem);
    block[rem.len()] = 0x80;
    if rem.len() >= 56 {
        compress(&mut state, &block, use_shani);
        block = [0u8; 64];
    }
    let bit_len = (data.len() as u64) * 8;
    block[56..64].copy_from_slice(&bit_len.to_be_bytes());
    compress(&mut state, &block, use_shani);

    let mut out = [0u8; 32];
    for i in 0..8 {
        out[i * 4..i * 4 + 4].copy_from_slice(&state[i].to_be_bytes());
    }
    out
}

const B64_LUT: [u8; 256] = {
    let mut t = [0xFFu8; 256];
    let mut i = 0;
    while i < 26 {
        t[(b'A' + i) as usize] = i;
        t[(b'a' + i) as usize] = i + 26;
        i += 1;
    }
    let mut d = 0;
    while d < 10 {
        t[(b'0' + d) as usize] = d + 52;
        d += 1;
    }
    t[b'-' as usize] = 62;
    t[b'_' as usize] = 63;
    t
};

#[inline]
pub fn b64url_decode(src: &[u8], dst: &mut [u8]) -> Option<usize> {
    let n = src.len();
    if n % 4 == 1 {
        return None;
    }
    let mut si = 0;
    let mut di = 0;
    while si + 4 <= n {
        let a = B64_LUT[src[si] as usize];
        let b = B64_LUT[src[si + 1] as usize];
        let c = B64_LUT[src[si + 2] as usize];
        let d = B64_LUT[src[si + 3] as usize];
        if (a | b | c | d) & 0xC0 != 0 {
            return None;
        }
        if di + 3 > dst.len() {
            return None;
        }
        dst[di] = (a << 2) | (b >> 4);
        dst[di + 1] = (b << 4) | (c >> 2);
        dst[di + 2] = (c << 6) | d;
        si += 4;
        di += 3;
    }
    let rem = n - si;
    if rem == 2 {
        let a = B64_LUT[src[si] as usize];
        let b = B64_LUT[src[si + 1] as usize];
        if (a | b) & 0xC0 != 0 || (b & 0x0F) != 0 || di + 1 > dst.len() {
            return None;
        }
        dst[di] = (a << 2) | (b >> 4);
        di += 1;
    } else if rem == 3 {
        let a = B64_LUT[src[si] as usize];
        let b = B64_LUT[src[si + 1] as usize];
        let c = B64_LUT[src[si + 2] as usize];
        if (a | b | c) & 0xC0 != 0 || (c & 0x03) != 0 || di + 2 > dst.len() {
            return None;
        }
        dst[di] = (a << 2) | (b >> 4);
        dst[di + 1] = (b << 4) | (c >> 2);
        di += 2;
    }
    Some(di)
}

pub struct JwtVerifier {
    ipad_state: [u32; 8],
    opad_state: [u32; 8],
    use_shani: bool,
    hdr_buf: Vec<u8>,
    pay_buf: Vec<u8>,
}

impl JwtVerifier {
    pub fn new(secret: &[u8]) -> Self {
        #[cfg(target_arch = "x86_64")]
        let mut use_shani = std::is_x86_feature_detected!("sha")
            && std::is_x86_feature_detected!("sse4.1")
            && std::is_x86_feature_detected!("ssse3");
        #[cfg(not(target_arch = "x86_64"))]
        let use_shani = false;

        if use_shani {
            let test_msg = b"twelve-dollar-challenge-shani-selftest-message-0123456789abcdef";
            let d_scalar = sha256_oneshot(test_msg, false);
            let d_shani = sha256_oneshot(test_msg, true);
            if d_scalar != d_shani {
                use_shani = false;
            }
        }

        let mut key_block = [0u8; 64];
        if secret.len() > 64 {
            let hashed = sha256_oneshot(secret, use_shani);
            key_block[..32].copy_from_slice(&hashed);
        } else {
            key_block[..secret.len()].copy_from_slice(secret);
        }

        let mut ipad = [0x36u8; 64];
        let mut opad = [0x5cu8; 64];
        for i in 0..64 {
            ipad[i] ^= key_block[i];
            opad[i] ^= key_block[i];
        }

        let mut ipad_state = H0;
        let mut opad_state = H0;
        compress(&mut ipad_state, &ipad, use_shani);
        compress(&mut opad_state, &opad, use_shani);

        Self {
            ipad_state,
            opad_state,
            use_shani,
            hdr_buf: vec![0u8; 4096],
            pay_buf: vec![0u8; 16384],
        }
    }

    #[inline(always)]
    fn hmac_sha256(&self, msg: &[u8]) -> [u8; 32] {
        let mut inner = self.ipad_state;
        let mut chunks = msg.chunks_exact(64);
        for chunk in &mut chunks {
            let block: &[u8; 64] = chunk.try_into().unwrap();
            compress(&mut inner, block, self.use_shani);
        }
        let rem = chunks.remainder();
        let mut block = [0u8; 64];
        block[..rem.len()].copy_from_slice(rem);
        block[rem.len()] = 0x80;
        if rem.len() >= 56 {
            compress(&mut inner, &block, self.use_shani);
            block = [0u8; 64];
        }
        let inner_bits = ((64 + msg.len()) as u64) * 8;
        block[56..64].copy_from_slice(&inner_bits.to_be_bytes());
        compress(&mut inner, &block, self.use_shani);

        let mut outer_block = [0u8; 64];
        for i in 0..8 {
            outer_block[i * 4..i * 4 + 4].copy_from_slice(&inner[i].to_be_bytes());
        }
        outer_block[32] = 0x80;
        let outer_bits: u64 = (64 + 32) * 8;
        outer_block[56..64].copy_from_slice(&outer_bits.to_be_bytes());

        let mut outer = self.opad_state;
        compress(&mut outer, &outer_block, self.use_shani);

        let mut out = [0u8; 32];
        for i in 0..8 {
            out[i * 4..i * 4 + 4].copy_from_slice(&outer[i].to_be_bytes());
        }
        out
    }

    #[inline]
    pub fn verify<'a>(
        &'a mut self,
        auth_header: Option<&[u8]>,
        now_s: f64,
    ) -> Result<(i64, Cow<'a, str>), &'static str> {
        let raw = auth_header.ok_or("missing bearer token")?;
        let token = raw
            .strip_prefix(b"Bearer ")
            .ok_or("missing bearer token")?;
        let bad = "invalid or expired token";
        if token.is_empty() || token.len() > 16384 {
            return Err(bad);
        }

        let dot1 = token.iter().position(|&b| b == b'.').ok_or(bad)?;
        let rest = &token[dot1 + 1..];
        let dot2_rel = rest.iter().position(|&b| b == b'.').ok_or(bad)?;
        let dot2 = dot1 + 1 + dot2_rel;
        let sig_b64 = &token[dot2 + 1..];
        if sig_b64.len() != 43 || sig_b64.contains(&b'.') {
            return Err(bad);
        }

        let mut sig = [0u8; 32];
        if b64url_decode(sig_b64, &mut sig) != Some(32) {
            return Err(bad);
        }

        let expected = self.hmac_sha256(&token[..dot2]);
        let mut diff = 0u8;
        for i in 0..32 {
            diff |= sig[i] ^ expected[i];
        }
        if diff != 0 {
            return Err(bad);
        }

        let hdr_b64 = &token[..dot1];
        if hdr_b64 != b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
            && hdr_b64 != b"eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9"
            && hdr_b64 != b"eyJhbGciOiJIUzI1NiJ9"
        {
            let hlen = b64url_decode(hdr_b64, &mut self.hdr_buf).ok_or(bad)?;
            parse_jwt_header(&self.hdr_buf[..hlen]).map_err(|_| bad)?;
        }

        let pay_b64 = &token[dot1 + 1..dot2];
        let plen = b64url_decode(pay_b64, &mut self.pay_buf).ok_or(bad)?;
        let claims = parse_jwt_claims(&self.pay_buf[..plen]).map_err(|_| bad)?;
        let exp = claims.exp.ok_or(bad)?;
        if exp <= now_s || claims.nbf.is_some_and(|nbf| nbf > now_s) {
            return Err(bad);
        }

        let bad_payload = "invalid token payload";
        let sub_str = claims.sub.ok_or(bad_payload)?;
        let uid = parse_positive_i64(&sub_str).ok_or(bad_payload)?;
        let username = claims.username.ok_or(bad_payload)?;
        Ok((uid, username))
    }
}

#[inline]
pub fn parse_positive_i64(s: &str) -> Option<i64> {
    let b = s.as_bytes();
    if b.is_empty() || b.len() > 19 {
        return None;
    }
    let mut acc: i64 = 0;
    for &c in b {
        if !c.is_ascii_digit() {
            return None;
        }
        acc = acc.checked_mul(10)?.checked_add((c - b'0') as i64)?;
    }
    if acc > 0 { Some(acc) } else { None }
}
