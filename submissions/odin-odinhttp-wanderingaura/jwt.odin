package main

import "core:crypto"
import "core:crypto/hmac"
import "core:encoding/json"
import "core:strconv"
import "core:strings"
import "core:time"

import http "http:."

// HS256 verification. The keyed HMAC state (the hashed ipad/opad blocks) is computed once and
// cloned per request; every token's signature is still checked on every request.

@(private = "file")
jwt_key: hmac.Context

jwt_init :: proc(secret: string) {
	hmac.init(&jwt_key, .SHA256, transmute([]byte)secret)
}

Auth :: struct {
	user_id:  i64,
	username: string,
}

Auth_Error :: enum {
	None,
	Missing, // 401 missing bearer token
	Invalid, // 401 invalid or expired token
	Payload, // 401 invalid token payload
}

@(private = "file")
b64url_value :: #force_inline proc(c: u8) -> u8 {
	switch c {
	case 'A' ..= 'Z': return c - 'A'
	case 'a' ..= 'z': return c - 'a' + 26
	case '0' ..= '9': return c - '0' + 52
	case '-':         return 62
	case '_':         return 63
	}
	return 0xFF
}

// Unpadded base64url into dst; false on a bad character, bad length or a too small dst.
@(private = "file")
b64url_decode :: proc(src: string, dst: []byte) -> (n: int, ok: bool) {
	if len(src) % 4 == 1 { return 0, false }
	if (len(src) * 3) / 4 > len(dst) { return 0, false }
	acc: u32
	bits: uint
	for c in transmute([]byte)src {
		v := b64url_value(c)
		if v == 0xFF { return 0, false }
		acc = (acc << 6) | u32(v)
		bits += 6
		if bits >= 8 {
			bits -= 8
			dst[n] = u8(acc >> bits)
			n += 1
		}
	}
	return n, true
}

@(private = "file")
decode_json_object :: proc(part: string) -> (obj: json.Object, ok: bool) {
	buf := make([]byte, len(part), context.temp_allocator)
	n := b64url_decode(part, buf) or_return
	v, err := json.parse_string(string(buf[:n]), .JSON, false, context.temp_allocator)
	if err != .None { return nil, false }
	return v.(json.Object)
}

verify_bearer :: proc(req: ^http.Request) -> (auth: Auth, err: Auth_Error) {
	header, has := http.headers_get_unsafe(req.headers, "authorization")
	if !has || !strings.has_prefix(header, "Bearer ") { return {}, .Missing }
	token := header[len("Bearer "):]

	d1 := strings.index_byte(token, '.')
	if d1 < 0 { return {}, .Invalid }
	d2 := strings.index_byte(token[d1 + 1:], '.')
	if d2 < 0 { return {}, .Invalid }
	d2 += d1 + 1
	signing_input := token[:d2]
	header_part, payload_part, sig_part := token[:d1], token[d1 + 1:d2], token[d2 + 1:]

	sig: [32]byte
	n, sig_ok := b64url_decode(sig_part, sig[:])
	if !sig_ok || n != len(sig) { return {}, .Invalid }

	ctx: hmac.Context
	hmac.clone(&ctx, &jwt_key)
	hmac.update(&ctx, transmute([]byte)signing_input)
	mac: [32]byte
	hmac.final(&ctx, mac[:])
	if crypto.compare_constant_time(mac[:], sig[:]) != 1 { return {}, .Invalid }

	head, head_ok := decode_json_object(header_part)
	if !head_ok { return {}, .Invalid }
	alg, alg_ok := head["alg"].(json.String)
	if !alg_ok || alg != "HS256" { return {}, .Invalid }

	payload, payload_ok := decode_json_object(payload_part)
	if !payload_ok { return {}, .Invalid }
	exp: f64
	#partial switch e in payload["exp"] {
	case json.Float:   exp = e
	case json.Integer: exp = f64(e)
	case:              return {}, .Invalid
	}
	if exp <= f64(time.time_to_unix(time.now())) { return {}, .Invalid }

	sub, sub_ok := payload["sub"].(json.String)
	if !sub_ok { return {}, .Payload }
	user_id, id_ok := parse_positive_id(sub)
	if !id_ok { return {}, .Payload }
	username, name_ok := payload["username"].(json.String)
	if !name_ok { return {}, .Payload }

	return {user_id = user_id, username = username}, .None
}

// A positive decimal integer of ASCII digits only ("0", "-1", "abc", "1.5" are not).
parse_positive_id :: proc(s: string) -> (id: i64, ok: bool) {
	if len(s) == 0 || len(s) > 18 { return 0, false }
	for c in transmute([]byte)s {
		if c < '0' || c > '9' { return 0, false }
	}
	v, _ := strconv.parse_i64_of_base(s, 10)
	return v, v > 0
}
