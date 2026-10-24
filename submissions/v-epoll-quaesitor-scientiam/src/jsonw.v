module main

// Append-only writers: every response is built by appending bytes to a reused buffer.
// Nothing here allocates.

const status_200 = 'HTTP/1.1 200 OK\r\n'
const status_201 = 'HTTP/1.1 201 Created\r\n'
const status_400 = 'HTTP/1.1 400 Bad Request\r\n'
const status_401 = 'HTTP/1.1 401 Unauthorized\r\n'
const status_404 = 'HTTP/1.1 404 Not Found\r\n'
const status_500 = 'HTTP/1.1 500 Internal Server Error\r\n'
const status_503 = 'HTTP/1.1 503 Service Unavailable\r\n'
const head_json = 'Content-Type: application/json\r\nContent-Length: '
const head_close = 'Connection: close\r\n'
const hex_digits = '0123456789abcdef'

@[inline]
fn put(mut out []u8, s string) {
	unsafe { out.push_many(s.str, s.len) }
}

@[inline]
fn put_bytes(mut out []u8, p &u8, n int) {
	if n > 0 {
		unsafe { out.push_many(p, n) }
	}
}

// json_int appends the decimal form of v.
fn json_int(mut out []u8, v i64) {
	if v == 0 {
		out << `0`
		return
	}
	mut n := u64(v)
	if v < 0 {
		out << `-`
		n = u64(-v)
	}
	mut tmp := [20]u8{}
	mut i := 20
	for n > 0 {
		i--
		tmp[i] = u8(`0` + n % 10)
		n /= 10
	}
	unsafe { out.push_many(&tmp[i], 20 - i) }
}

// has_json_special reports whether any byte of w is below 0x20, `"` (0x22) or `\` (0x5c),
// with the usual SWAR zero-byte test applied to each candidate.
@[inline]
fn has_json_special(w u64) bool {
	ones := u64(0x0101010101010101)
	highs := u64(0x8080808080808080)
	// bytes < 0x20: subtracting 0x20 borrows out of the top bit of exactly those bytes
	low := (w - ones * 0x20) & ~w & highs
	q := w ^ (ones * 0x22)
	quote := (q - ones) & ~q & highs
	b := w ^ (ones * 0x5c)
	back := (b - ones) & ~b & highs
	return (low | quote | back) != 0
}

// json_str appends a quoted JSON string. Only `"`, `\` and control characters are
// escaped; UTF-8 is copied through untouched (the golden files contain none of either).
fn json_str(mut out []u8, p &u8, n int) {
	out << `"`
	mut run := 0
	for i := 0; i < n; i++ {
		// skip 8 bytes at a time while none of them is a quote, a backslash or a control
		for i + 8 <= n {
			w := unsafe { *(&u64(p + i)) }
			if has_json_special(w) {
				break
			}
			i += 8
		}
		if i >= n {
			break
		}
		c := unsafe { p[i] }
		if c >= 0x20 && c != `"` && c != `\\` {
			continue
		}
		put_bytes(mut out, unsafe { p + run }, i - run)
		run = i + 1
		match c {
			`"` { put(mut out, '\\"') }
			`\\` { put(mut out, '\\\\') }
			`\n` { put(mut out, '\\n') }
			`\r` { put(mut out, '\\r') }
			`\t` { put(mut out, '\\t') }
			`\b` { put(mut out, '\\b') }
			`\f` { put(mut out, '\\f') }
			else {
				put(mut out, '\\u00')
				out << hex_digits[c >> 4]
				out << hex_digits[c & 15]
			}
		}
	}
	put_bytes(mut out, unsafe { p + run }, n - run)
	out << `"`
}

// json_post appends one post object in the spec's key order.
fn json_post(mut out []u8, id i64, body &u8, body_len int, created &u8, created_len int, author &u8, author_len int, likes i64) {
	put(mut out, '{"id":')
	json_int(mut out, id)
	put(mut out, ',"body":')
	json_str(mut out, body, body_len)
	put(mut out, ',"created_at":')
	json_str(mut out, created, created_len)
	put(mut out, ',"author":')
	json_str(mut out, author, author_len)
	put(mut out, ',"like_count":')
	json_int(mut out, likes)
	out << `}`
}

// json_post_row appends the post in the current row of st (the sql_select_post columns).
fn json_post_row(mut out []u8, st &C.sqlite3_stmt) {
	id := C.sqlite3_column_int64(st, 0)
	body := C.sqlite3_column_text(st, 1)
	body_len := C.sqlite3_column_bytes(st, 1)
	created := C.sqlite3_column_text(st, 2)
	created_len := C.sqlite3_column_bytes(st, 2)
	author := C.sqlite3_column_text(st, 3)
	author_len := C.sqlite3_column_bytes(st, 3)
	likes := C.sqlite3_column_int64(st, 4)
	json_post(mut out, id, body, body_len, created, created_len, author, author_len, likes)
}

// put_response appends a complete HTTP/1.1 response: status line, JSON headers and body.
fn put_response(mut out []u8, status string, body []u8, close bool) {
	put(mut out, status)
	put(mut out, head_json)
	json_int(mut out, body.len)
	put(mut out, '\r\n')
	if close {
		put(mut out, head_close)
	}
	put(mut out, '\r\n')
	put_bytes(mut out, body.data, body.len)
}

// put_error appends an `{"error":"<msg>"}` response. msg must need no escaping.
fn put_error(mut out []u8, status string, msg string, close bool) {
	put(mut out, status)
	put(mut out, head_json)
	json_int(mut out, msg.len + 12)
	put(mut out, '\r\n')
	if close {
		put(mut out, head_close)
	}
	put(mut out, '\r\n{"error":"')
	put(mut out, msg)
	put(mut out, '"}')
}
