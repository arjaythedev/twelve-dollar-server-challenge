module main

import crypto.hmac
import crypto.sha256

// AuthError is the 401 the token check produced, in the spec's precedence order.
enum AuthError {
	missing // no `Authorization: Bearer ...` header
	invalid // bad signature, expired, not HS256, malformed
	payload // valid signature, but `sub`/`username` are not usable
}

// Claims is what a verified token grants: the user id and the username bytes, which live
// in App.user scratch until the next request.
struct Claims {
	user_id  i64
	username []u8
}

const b64url_table = build_b64url_table()

fn build_b64url_table() [256]i16 {
	mut t := [256]i16{init: -1}
	for i in 0 .. 26 {
		t[`A` + i] = i16(i)
		t[`a` + i] = i16(26 + i)
	}
	for i in 0 .. 10 {
		t[`0` + i] = i16(52 + i)
	}
	t[`-`] = 62
	t[`_`] = 63
	return t
}

// b64url_decode appends the decoded bytes of src (base64url, padding optional) to dst.
fn b64url_decode(src []u8, mut dst []u8) bool {
	mut n := src.len
	for n > 0 && src[n - 1] == `=` {
		n--
	}
	if n % 4 == 1 {
		return false
	}
	mut acc := u32(0)
	mut bits := 0
	for i in 0 .. n {
		v := b64url_table[src[i]]
		if v < 0 {
			return false
		}
		acc = acc << 6 | u32(v)
		bits += 6
		if bits >= 8 {
			bits -= 8
			dst << u8(acc >> bits & 0xFF)
		}
	}
	return true
}

fn (mut app App) verify_token(tok []u8) !Claims {
	mut d1 := -1
	mut d2 := -1
	for i, c in tok {
		if c == `.` {
			if d1 < 0 {
				d1 = i
			} else if d2 < 0 {
				d2 = i
			} else {
				return error_with_code('invalid', int(AuthError.invalid))
			}
		}
	}
	if d1 <= 0 || d2 <= d1 + 1 || d2 + 1 >= tok.len {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	// signature first: it is the cheapest full rejection of a forged token
	app.sig.clear()
	if !b64url_decode(tok[d2 + 1..], mut app.sig) || app.sig.len != 32 {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	digest := hmac.new(app.secret, tok[..d2], sha256.sum, sha256.block_size)
	if !hmac.equal(app.sig, digest) {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	// header: must say HS256 and nothing else matters
	app.scratch.clear()
	if !b64url_decode(tok[..d1], mut app.scratch) {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	if !header_is_hs256(app.scratch) {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	// payload
	app.scratch.clear()
	if !b64url_decode(tok[d1 + 1..d2], mut app.scratch) {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	mut w := new_object_walker(app.scratch) or {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	mut exp := i64(-1)
	mut has_exp := false
	mut sub_ok := false
	mut sub := i64(0)
	mut has_user := false
	app.user.clear()
	for {
		more := w.next() or { return error_with_code('invalid', int(AuthError.invalid)) }
		if !more {
			break
		}
		if w.key('exp') {
			exp = w.read_int() or { return error_with_code('invalid', int(AuthError.invalid)) }
			has_exp = true
		} else if w.key('sub') {
			sub_ok = false
			if w.value_is_string() {
				w.read_string(mut app.text) or {
					return error_with_code('invalid', int(AuthError.invalid))
				}
				sub = parse_positive_int(app.text)
				sub_ok = sub > 0
			} else {
				w.skip() or { return error_with_code('invalid', int(AuthError.invalid)) }
			}
		} else if w.key('username') {
			has_user = false
			if w.value_is_string() {
				w.read_string(mut app.user) or {
					return error_with_code('invalid', int(AuthError.invalid))
				}
				has_user = true
			} else {
				w.skip() or { return error_with_code('invalid', int(AuthError.invalid)) }
			}
		} else {
			w.skip() or { return error_with_code('invalid', int(AuthError.invalid)) }
		}
	}
	if !has_exp || exp <= app.now_unix() {
		return error_with_code('invalid', int(AuthError.invalid))
	}
	if !sub_ok || !has_user {
		return error_with_code('payload', int(AuthError.payload))
	}
	return Claims{
		user_id:  sub
		username: app.user
	}
}

// header_is_hs256 walks the decoded JOSE header and requires `"alg":"HS256"`.
fn header_is_hs256(b []u8) bool {
	mut w := new_object_walker(b) or { return false }
	mut ok := false
	mut alg := []u8{cap: 8}
	for {
		more := w.next() or { return false }
		if !more {
			break
		}
		if w.key('alg') {
			if !w.value_is_string() {
				return false
			}
			w.read_string(mut alg) or { return false }
			ok = alg.len == 5 && alg[0] == `H` && alg[1] == `S` && alg[2] == `2` && alg[3] == `5`
				&& alg[4] == `6`
		} else {
			w.skip() or { return false }
		}
	}
	return ok
}

// parse_positive_int parses an all-digit string into a positive i64 (0 when it is not one).
fn parse_positive_int(s []u8) i64 {
	if s.len == 0 || s.len > 18 {
		return 0
	}
	mut v := i64(0)
	for c in s {
		if c < `0` || c > `9` {
			return 0
		}
		v = v * 10 + i64(c - `0`)
	}
	return v
}
