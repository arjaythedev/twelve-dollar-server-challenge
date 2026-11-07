use std::fs;
use std::io::{BufRead, BufReader, Read};
use std::net::{Shutdown, TcpListener};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{SystemTime, UNIX_EPOCH};

#[test]
fn sleeping_vu_does_not_count_a_peer_half_close_as_live() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let peer = thread::spawn(move || listener.accept().unwrap().0);
    let path = std::env::temp_dir().join(format!(
        "loadgen-tokens-{}-{}.json",
        std::process::id(),
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()
    ));
    fs::write(&path, r#"[{"token":"test","username":"test"}]"#).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_loadgen"))
        .args(["127.0.0.1", &port.to_string(), "1", "0", "2"])
        .arg(&path)
        .args(["0", "30", "0"])
        .env("LG_PLATEAU_DELAY_S", "0.1")
        .env_remove("LG_START_UNIX_MS")
        .env_remove("LG_HISTOGRAM_PATH")
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let socket = peer.join().unwrap();
    let mut output = BufReader::new(child.stdout.take().unwrap());
    let mut text = String::new();
    loop {
        let mut line = String::new();
        assert_ne!(output.read_line(&mut line).unwrap(), 0, "{text}");
        let reached = line.starts_with("target-reached ");
        text.push_str(&line);
        if reached { break; }
    }
    socket.shutdown(Shutdown::Write).unwrap();
    output.read_to_string(&mut text).unwrap();
    let status = child.wait().unwrap();
    fs::remove_file(path).unwrap();
    assert_eq!(status.code(), Some(99), "{text}");
    assert!(text.contains("min_active=0"), "{text}");
}
