module main

import crypto.hmac
import crypto.sha256
import net
import os
import time

// Build-time PGO training (`bin/server --train <db-path>`): creates a small synthetic
// database (never the challenge seed), serves it on a loopback port from a second thread,
// and sends a few thousand requests over every route through real sockets, so the
// profile covers the event loop, parsing, JWT, SQLite and JSON output. Then it exits.

const train_users = 2000
const train_posts = 20000
const train_likes = 60000
const train_rounds = 4000
const train_port = 38080

const train_schema = "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));" +
	"CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));" +
	"CREATE TABLE likes (user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id), created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')), PRIMARY KEY (user_id, post_id));" +
	'CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);' +
	'CREATE INDEX posts_user_id_idx ON posts (user_id);' +
	'CREATE INDEX likes_post_id_idx ON likes (post_id);'

fn train_make_db(path string) ! {
	os.rm(path) or {}
	os.rm(path + '-wal') or {}
	os.rm(path + '-shm') or {}
	mut conn := &C.sqlite3(unsafe { nil })
	if C.sqlite3_open_v2(&char(path.str), &conn, sqlite_open_readwrite | 0x4, unsafe { nil }) != sqlite_ok {
		return error('train: cannot create ${path}')
	}
	mut script := 'PRAGMA journal_mode=WAL;PRAGMA synchronous=OFF;BEGIN;' + train_schema
	for u in 1 .. train_users + 1 {
		script += "INSERT INTO users(id,username) VALUES(${u},'user_${u}');"
	}
	mut seed := u32(12345)
	for p in 1 .. train_posts + 1 {
		seed = seed * 1103515245 + 12345
		u := int(seed >> 8) % train_users + 1
		body := 'training post ${p} with some words so the JSON writer has text to copy'
		ts := 1735689600 + p * 60
		script += "INSERT INTO posts(id,user_id,body,created_at) VALUES(${p},${u},'${body}',strftime('%Y-%m-%dT%H:%M:%fZ',${ts},'unixepoch'));"
	}
	for _ in 0 .. train_likes {
		seed = seed * 1103515245 + 12345
		u := int(seed >> 8) % train_users + 1
		seed = seed * 1103515245 + 12345
		p := int(seed >> 8) % train_posts + 1
		script += 'INSERT OR IGNORE INTO likes(user_id,post_id) VALUES(${u},${p});'
	}
	script += 'COMMIT;'
	if C.sqlite3_exec(conn, &char(script.str), unsafe { nil }, unsafe { nil }, unsafe { nil }) != sqlite_ok {
		return error('train: fill failed: ${unsafe { cstring_to_vstring(C.sqlite3_errmsg(conn)) }}')
	}
	C.sqlite3_close(conn)
}

const b64url_chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'

fn b64url_encode(src []u8) string {
	mut out := []u8{cap: (src.len * 4 + 2) / 3 + 1}
	mut i := 0
	for i + 3 <= src.len {
		n := u32(src[i]) << 16 | u32(src[i + 1]) << 8 | u32(src[i + 2])
		out << b64url_chars[n >> 18 & 63]
		out << b64url_chars[n >> 12 & 63]
		out << b64url_chars[n >> 6 & 63]
		out << b64url_chars[n & 63]
		i += 3
	}
	if src.len - i == 2 {
		n := u32(src[i]) << 16 | u32(src[i + 1]) << 8
		out << b64url_chars[n >> 18 & 63]
		out << b64url_chars[n >> 12 & 63]
		out << b64url_chars[n >> 6 & 63]
	} else if src.len - i == 1 {
		n := u32(src[i]) << 16
		out << b64url_chars[n >> 18 & 63]
		out << b64url_chars[n >> 12 & 63]
	}
	return out.bytestr()
}

fn train_token(secret []u8, user int) string {
	head := b64url_encode('{"alg":"HS256","typ":"JWT"}'.bytes())
	now := time.now().unix()
	payload := b64url_encode('{"sub":"${user}","username":"user_${user}","iat":${now},"exp":${now + 3600}}'.bytes())
	signing := '${head}.${payload}'
	sig := hmac.new(secret, signing.bytes(), sha256.sum, sha256.block_size)
	return '${signing}.${b64url_encode(sig)}'
}

fn train_server(db string) {
	os.setenv('SQLITE_PATH', db, true)
	serve('127.0.0.1', train_port) or {
		eprintln('train server: ${err}')
		exit(1)
	}
}

// train_request sends one request and reads the whole response (Content-Length framed).
fn train_request(mut conn net.TcpConn, req string, mut buf []u8) ! {
	conn.write_string(req)!
	mut have := 0
	for {
		n := conn.read(mut buf[have..]) or { return error('read: ${err}') }
		if n <= 0 {
			return error('closed')
		}
		have += n
		total := frame_len(buf[..have])
		if total > 0 && have >= total {
			return
		}
		if have >= buf.len {
			return error('response too large')
		}
	}
}

// frame_len returns the length of a complete response in buf, or -1.
fn frame_len(buf []u8) int {
	for i := 0; i + 3 < buf.len; i++ {
		if buf[i] == `\r` && buf[i + 1] == `\n` && buf[i + 2] == `\r` && buf[i + 3] == `\n` {
			head := buf[..i].bytestr()
			idx := head.to_lower().index('content-length:') or { return i + 4 }
			n := head[idx + 15..].trim_space().split('\r')[0].int()
			return i + 4 + n
		}
	}
	return -1
}

fn run_training(db string) ! {
	train_make_db(db)!
	secret := os.getenv('JWT_SECRET').bytes()
	spawn train_server(db)
	mut conn := &net.TcpConn(unsafe { nil })
	for _ in 0 .. 100 {
		time.sleep(50 * time.millisecond)
		conn = net.dial_tcp('127.0.0.1:${train_port}') or { continue }
		break
	}
	if conn == unsafe { nil } {
		return error('train: server did not start')
	}
	conn.set_read_timeout(5 * time.second)
	mut buf := []u8{len: 64 * 1024}
	mut tokens := []string{cap: 64}
	for u in 1 .. 65 {
		tokens << train_token(secret, u)
	}
	bad := 'eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.AAAA'
	mut seed := u32(777)
	for round in 0 .. train_rounds {
		seed = seed * 1103515245 + 12345
		post_id := int(seed >> 8) % train_posts + 1
		tok := tokens[round % tokens.len]
		train_request(mut conn, 'GET /feed HTTP/1.1\r\nHost: t\r\n\r\n', mut buf)!
		train_request(mut conn, 'GET /posts/${post_id} HTTP/1.1\r\nHost: t\r\n\r\n', mut buf)!
		if round % 7 == 0 {
			train_request(mut conn, 'POST /posts/${post_id}/like HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer ${tok}\r\nContent-Length: 0\r\n\r\n', mut buf)!
		}
		if round % 50 == 0 {
			body := '{"body":"  round ${round} says hi \\"quoted\\" caf\\u00e9 "}'
			train_request(mut conn, 'POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer ${tok}\r\nContent-Type: application/json\r\nContent-Length: ${body.len}\r\n\r\n${body}', mut buf)!
		}
		if round % 97 == 0 {
			train_request(mut conn, 'GET /health HTTP/1.1\r\nHost: t\r\n\r\n', mut buf)!
			train_request(mut conn, 'GET /posts/abc HTTP/1.1\r\nHost: t\r\n\r\n', mut buf)!
			train_request(mut conn, 'GET /nope HTTP/1.1\r\nHost: t\r\n\r\n', mut buf)!
			train_request(mut conn, 'POST /posts/${post_id}/like HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer ${bad}\r\nContent-Length: 0\r\n\r\n', mut buf)!
			train_request(mut conn, 'POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n', mut buf)!
		}
	}
	conn.close()!
	println('training done: ${train_rounds} rounds')
}
