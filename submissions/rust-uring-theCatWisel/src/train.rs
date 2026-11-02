use crate::uring::Server;
use crate::{astraea_eng_open, astraea_make_train_db};
use std::ffi::CString;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

pub static TRAINING_DONE: AtomicBool = AtomicBool::new(false);

fn b64url_encode(input: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::with_capacity((input.len() * 4 + 2) / 3);
    let mut i = 0;
    while i + 3 <= input.len() {
        let n = ((input[i] as u32) << 16) | ((input[i + 1] as u32) << 8) | (input[i + 2] as u32);
        out.push(ALPHABET[((n >> 18) & 63) as usize] as char);
        out.push(ALPHABET[((n >> 12) & 63) as usize] as char);
        out.push(ALPHABET[((n >> 6) & 63) as usize] as char);
        out.push(ALPHABET[(n & 63) as usize] as char);
        i += 3;
    }
    let rem = input.len() - i;
    if rem == 1 {
        let n = (input[i] as u32) << 16;
        out.push(ALPHABET[((n >> 18) & 63) as usize] as char);
        out.push(ALPHABET[((n >> 12) & 63) as usize] as char);
    } else if rem == 2 {
        let n = ((input[i] as u32) << 16) | ((input[i + 1] as u32) << 8);
        out.push(ALPHABET[((n >> 18) & 63) as usize] as char);
        out.push(ALPHABET[((n >> 12) & 63) as usize] as char);
        out.push(ALPHABET[((n >> 6) & 63) as usize] as char);
    }
    out
}

pub fn run_pgo_training(nslots: u32) -> ! {
    let db_path = format!("/tmp/astraea_pgo_{}.db", std::process::id());
    let c_path = CString::new(db_path.clone()).unwrap();
    unsafe {
        if astraea_make_train_db(c_path.as_ptr(), 1500, 12000, 48000) != 0 {
            eprintln!("pgo: astraea_make_train_db failed");
            std::process::exit(1);
        }
    }

    let port: u16 = 39182;
    let secret = b"twelve-dollar-challenge";
    let eng = unsafe { astraea_eng_open(c_path.as_ptr(), 8192) };
    if eng.is_null() {
        eprintln!("pgo: astraea_eng_open failed");
        std::process::exit(1);
    }

    thread::spawn(move || {
        thread::sleep(Duration::from_millis(80));
        // Build a valid HS256 token for user 1
        let header_b64 = b64url_encode(br#"{"alg":"HS256","typ":"JWT"}"#);
        let payload_b64 = b64url_encode(
            br#"{"sub":"1","username":"user_1","iat":1700000000,"exp":4102444800}"#,
        );
        let signing_input = format!("{header_b64}.{payload_b64}");
        // Compute signature via openssl-equivalent or our own HMAC-SHA256
        let sig_bytes = compute_hmac_sha256(secret, signing_input.as_bytes());
        let token = format!("{signing_input}.{}", b64url_encode(&sig_bytes));

        let mut streams = Vec::with_capacity(16);
        for _ in 0..16 {
            if let Ok(s) = TcpStream::connect(("127.0.0.1", port)) {
                let _ = s.set_nodelay(true);
                streams.push(s);
            }
        }

        let mut buf = [0u8; 16384];
        let n_streams = streams.len();
        for iter in 0..1200usize {
            let s = &mut streams[iter % n_streams];
            let req = match iter % 10 {
                0..=3 => "GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n".to_string(),
                4..=7 => format!(
                    "GET /posts/{} HTTP/1.1\r\nHost: localhost\r\n\r\n",
                    1 + (iter * 17) % 12000
                ),
                8 => format!(
                    "POST /posts/{}/like HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Length: 0\r\n\r\n",
                    1 + (iter * 31) % 12000
                ),
                _ => {
                    let body = r#"{"body":"training workload post body with \"quotes\" and unicode ✓"}"#;
                    format!(
                        "POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
                        body.len()
                    )
                }
            };
            if s.write_all(req.as_bytes()).is_ok() {
                let _ = s.read(&mut buf);
            }
        }

        drop(streams);
        TRAINING_DONE.store(true, Ordering::Release);
        let _ = TcpStream::connect(("127.0.0.1", port));
        thread::sleep(Duration::from_secs(10));
    });

    let mut srv = Server::new("127.0.0.1", port, eng, secret, nslots, 1).expect("pgo server");
    srv.run();
    let _ = std::fs::remove_file(&db_path);
    let _ = std::fs::remove_file(format!("{db_path}-wal"));
    let _ = std::fs::remove_file(format!("{db_path}-shm"));
    std::process::exit(0);
}

fn compute_hmac_sha256(key: &[u8], msg: &[u8]) -> [u8; 32] {
    // Use sha256 via simple scalar helper for training token generation
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    fn sha256(data: &[u8]) -> [u8; 32] {
        let mut s: [u32; 8] = [
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
            0x5be0cd19,
        ];
        let mut msg = data.to_vec();
        let bit_len = (data.len() as u64) * 8;
        msg.push(0x80);
        while (msg.len() % 64) != 56 {
            msg.push(0);
        }
        msg.extend_from_slice(&bit_len.to_be_bytes());
        for chunk in msg.chunks_exact(64) {
            let mut w = [0u32; 64];
            for i in 0..16 {
                w[i] = u32::from_be_bytes([
                    chunk[i * 4],
                    chunk[i * 4 + 1],
                    chunk[i * 4 + 2],
                    chunk[i * 4 + 3],
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
            let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h) =
                (s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
            for i in 0..64 {
                let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
                let ch = (e & f) ^ ((!e) & g);
                let t1 = h
                    .wrapping_add(s1)
                    .wrapping_add(ch)
                    .wrapping_add(K[i])
                    .wrapping_add(w[i]);
                let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
                let maj = (a & b) ^ (a & c) ^ (b & c);
                let t2 = s0.wrapping_add(maj);
                h = g;
                g = f;
                f = e;
                e = d.wrapping_add(t1);
                d = c;
                c = b;
                b = a;
                a = t1.wrapping_add(t2);
            }
            s[0] = s[0].wrapping_add(a);
            s[1] = s[1].wrapping_add(b);
            s[2] = s[2].wrapping_add(c);
            s[3] = s[3].wrapping_add(d);
            s[4] = s[4].wrapping_add(e);
            s[5] = s[5].wrapping_add(f);
            s[6] = s[6].wrapping_add(g);
            s[7] = s[7].wrapping_add(h);
        }
        let mut out = [0u8; 32];
        for i in 0..8 {
            out[i * 4..i * 4 + 4].copy_from_slice(&s[i].to_be_bytes());
        }
        out
    }

    let mut k_block = [0u8; 64];
    k_block[..key.len()].copy_from_slice(key);
    let mut inner = Vec::with_capacity(64 + msg.len());
    for i in 0..64 {
        inner.push(k_block[i] ^ 0x36);
    }
    inner.extend_from_slice(msg);
    let inner_hash = sha256(&inner);
    let mut outer = Vec::with_capacity(64 + 32);
    for i in 0..64 {
        outer.push(k_block[i] ^ 0x5c);
    }
    outer.extend_from_slice(&inner_hash);
    sha256(&outer)
}
