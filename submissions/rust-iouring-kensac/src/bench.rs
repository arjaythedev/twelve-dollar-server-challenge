//! Microbenchmark of the per-request user-space path (cargo build --features bench; BENCH=1).
use crate::app::App;
use crate::http;
use std::hint::black_box;
use std::time::Instant;

fn time<F: FnMut()>(name: &str, iters: u32, mut f: F) {
    for _ in 0..iters / 10 {
        f();
    }
    let mut best = f64::MAX;
    for _ in 0..5 {
        let t = Instant::now();
        for _ in 0..iters {
            f();
        }
        best = best.min(t.elapsed().as_nanos() as f64 / iters as f64);
    }
    println!("{name:<28} {best:>9.1} ns");
}

pub fn run(app: &mut App, token: &str) {
    let feed = b"GET /feed HTTP/1.1\r\nHost: 127.0.0.1:3000\r\nUser-Agent: k6/0.52.0 (https://k6.io/)\r\nAccept-Encoding: gzip\r\n\r\n".to_vec();
    let post = b"GET /posts/12345 HTTP/1.1\r\nHost: 127.0.0.1:3000\r\nUser-Agent: k6/0.52.0 (https://k6.io/)\r\nAccept-Encoding: gzip\r\n\r\n".to_vec();
    let like = format!("POST /posts/12345/like HTTP/1.1\r\nHost: 127.0.0.1:3000\r\nUser-Agent: k6/0.52.0 (https://k6.io/)\r\nContent-Length: 0\r\nAuthorization: Bearer {token}\r\nContent-Type: application/json\r\nAccept-Encoding: gzip\r\n\r\n").into_bytes();
    let auth = format!("Bearer {token}").into_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(8192);

    let mut once = |req: &[u8], out: &mut Vec<u8>| {
        out.clear();
        if let http::Parsed::Done(r, _) = http::parse(req) {
            app.handle(&r, out);
        }
    };
    {
        // Cur must match escape_into / push_int byte for byte.
        let mut x: u64 = 0x9e3779b97f4a7c15;
        let alphabet: &[u8] = b"ab\"\\\n\r\t\x08\x0c\x01\x1f /\xc3\xa9zZ09";
        for len in 0..200usize {
            for _ in 0..50 {
                let s: Vec<u8> = (0..len)
                    .map(|_| {
                        x ^= x << 13;
                        x ^= x >> 7;
                        x ^= x << 17;
                        if x % 4 == 0 { alphabet[(x >> 8) as usize % alphabet.len()] } else { b'a' + (x >> 8) as u8 % 26 }
                    })
                    .collect();
                let mut a = vec![1u8, 2];
                crate::json::escape_into(&mut a, &s);
                let v = (x as i64) >> (x % 64);
                crate::json::push_int(&mut a, v);
                let mut b = vec![1u8, 2];
                crate::json::fill(&mut b, crate::json::esc_bound(s.len()) + 20, |c| {
                    c.esc(&s);
                    c.int(v);
                });
                assert_eq!(a, b, "mismatch len {len}");
            }
        }
        for v in [0i64, 9, 10, 99, 100, 999, 1000, i64::MAX, i64::MIN, -1, -10] {
            let mut a = Vec::new();
            crate::json::push_int(&mut a, v);
            let mut b = Vec::new();
            crate::json::fill(&mut b, 20, |c| c.int(v));
            assert_eq!(a, b);
        }
        println!("Cur equivalence ok");
    }
    once(&feed, &mut out);
    println!("feed response {} bytes", out.len());
    once(&post, &mut out);
    println!("post response {} bytes", out.len());

    time("parse GET /feed", 2_000_000, || {
        black_box(matches!(http::parse(black_box(&feed)), http::Parsed::Done(..)));
    });
    time("parse POST like", 2_000_000, || {
        black_box(matches!(http::parse(black_box(&like)), http::Parsed::Done(..)));
    });
    time("jwt verify", 500_000, || {
        black_box(app.bench_verify(black_box(&auth)));
    });
    {
        use sha2::digest::{FixedOutput, Update};
        let inner = sha2::Sha256::default().chain([0x36u8; 64]);
        let outer = sha2::Sha256::default().chain([0x5cu8; 64]);
        let signed = token.rsplit_once('.').unwrap().0.as_bytes().to_vec();
        println!("signed {} bytes", signed.len());
        time("hmac only", 500_000, || {
            let ih = inner.clone().chain(black_box(&signed)).finalize_fixed();
            black_box(outer.clone().chain(&ih).finalize_fixed());
        });
    }
    {
        out.clear();
        let _ = app.db.feed(&mut out);
        let body = out.clone();
        let pieces: Vec<&[u8]> = body.split(|&c| c == b'"').collect();
        let total: usize = pieces.iter().map(|p| p.len()).sum();
        println!("{} pieces, {} bytes", pieces.len(), total);
        let mut o: Vec<u8> = Vec::with_capacity(16384);
        time("escape feed pieces", 200_000, || {
            o.clear();
            for p in &pieces {
                crate::json::escape_into(&mut o, black_box(p));
            }
            black_box(o.len());
        });
        time("memcpy feed pieces", 200_000, || {
            o.clear();
            for p in &pieces {
                o.extend_from_slice(black_box(p));
            }
            black_box(o.len());
        });
        time("push_int x40", 200_000, || {
            o.clear();
            for i in 0..40 {
                crate::json::push_int(&mut o, black_box(1234567 + i));
            }
            black_box(o.len());
        });
    }
    let _ = app.db.feed(&mut out);
    time("feed_json only", 200_000, || {
        out.clear();
        app.db.bench_feed_json(&mut out);
        black_box(out.len());
    });
    let mut full = Vec::new();
    let b0 = { let _ = app.db.feed(&mut full); full.clone() };
    time("finish-like full copy", 200_000, || {
        out.clear();
        out.extend_from_slice(black_box(&b0));
        black_box(out.len());
    });
    time("db.feed only", 50_000, || {
        out.clear();
        black_box(app.db.feed(&mut out).is_ok());
    });
    time("parse+handle GET /feed", 50_000, || {
        out.clear();
        if let http::Parsed::Done(r, _) = http::parse(black_box(&feed)) {
            app.handle(&r, &mut out);
        }
        black_box(out.len());
    });
    time("db.post only", 500_000, || {
        out.clear();
        black_box(app.db.post(12345, &mut out).is_ok());
    });
    time("parse+handle GET /posts/:id", 500_000, || {
        out.clear();
        if let http::Parsed::Done(r, _) = http::parse(black_box(&post)) {
            app.handle(&r, &mut out);
        }
        black_box(out.len());
    });
    let mut n = 0u32;
    time("parse+handle POST like", 200_000, || {
        out.clear();
        if let http::Parsed::Done(r, _) = http::parse(black_box(&like)) {
            app.handle(&r, &mut out);
        }
        n += 1;
        if n % 256 == 0 {
            app.db.commit();
        }
        black_box(out.len());
    });
    app.db.commit();
}
