// Development load generator: closed-loop keep-alive clients, one thread each,
// with a microsecond latency histogram. Not part of the submission.
//
//   rustc -O bench/load.rs -o build/load
//   ./build/load <port> <connections> <seconds> <path>
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

const BUCKETS: usize = 1 << 22; // 4M µs = 4 s, 1 µs resolution

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port: u16 = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(3000);
    let conns: usize = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(64);
    let secs: u64 = args.get(3).map(|s| s.parse().unwrap()).unwrap_or(10);
    let path: String = args.get(4).cloned().unwrap_or_else(|| String::from("/feed"));

    let req = format!("GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n");
    let hist: Arc<Vec<AtomicU64>> = Arc::new((0..BUCKETS).map(|_| AtomicU64::new(0)).collect());
    let errors = Arc::new(AtomicU64::new(0));
    let mut handles = Vec::new();

    for _ in 0..conns {
        let req = req.clone();
        let hist = hist.clone();
        let errors = errors.clone();
        handles.push(std::thread::spawn(move || {
            let mut s = match TcpStream::connect(("127.0.0.1", port)) {
                Ok(s) => s,
                Err(_) => {
                    errors.fetch_add(1, Ordering::Relaxed);
                    return;
                }
            };
            let _ = s.set_nodelay(true);
            let mut buf = vec![0u8; 64 * 1024];
            loop {
                let t0 = Instant::now();
                if s.write_all(req.as_bytes()).is_err() {
                    errors.fetch_add(1, Ordering::Relaxed);
                    return;
                }
                let mut have = 0usize;
                let mut need = usize::MAX;
                loop {
                    if need != usize::MAX && have >= need {
                        break;
                    }
                    let n = match s.read(&mut buf[have..]) {
                        Ok(0) => return,
                        Ok(n) => n,
                        Err(_) => {
                            errors.fetch_add(1, Ordering::Relaxed);
                            return;
                        }
                    };
                    have += n;
                    if need == usize::MAX {
                        if let Some(pos) = find(&buf[..have], b"\r\n\r\n") {
                            if let Some(cl) = content_length(&buf[..pos]) {
                                need = pos + 4 + cl;
                            }
                        }
                    }
                }
                let us = t0.elapsed().as_micros() as usize;
                hist[us.min(BUCKETS - 1)].fetch_add(1, Ordering::Relaxed);
            }
        }));
    }

    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(secs) {
        std::thread::sleep(Duration::from_millis(100));
    }
    let elapsed = start.elapsed().as_secs_f64();
    let mut counts = Vec::with_capacity(BUCKETS);
    let mut total = 0u64;
    for b in hist.iter() {
        let v = b.load(Ordering::Relaxed);
        total += v;
        counts.push(v);
    }
    let pct = |p: f64| -> usize {
        let target = (total as f64 * p) as u64;
        let mut acc = 0u64;
        for (i, &c) in counts.iter().enumerate() {
            acc += c;
            if acc >= target {
                return i;
            }
        }
        BUCKETS - 1
    };
    println!(
        "{path} conns={conns} reqs={total} rps={:.0} err={} p50={}us p95={}us p99={}us p999={}us max={}us",
        total as f64 / elapsed,
        errors.load(Ordering::Relaxed),
        pct(0.50),
        pct(0.95),
        pct(0.99),
        pct(0.999),
        pct(0.9999)
    );
    std::process::exit(0);
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.len() > hay.len() {
        return None;
    }
    (0..=hay.len() - needle.len()).find(|&i| &hay[i..i + needle.len()] == needle)
}

fn content_length(head: &[u8]) -> Option<usize> {
    let text = String::from_utf8_lossy(head);
    for line in text.split("\r\n") {
        let lower = line.to_ascii_lowercase();
        if let Some(rest) = lower.strip_prefix("content-length:") {
            return rest.trim().parse().ok();
        }
    }
    None
}
