# JWT HS256 verification: base64url, signature check, header and payload checks.
#
# Result codes match SPEC.md's error table:
#   0 ok, 1 "invalid or expired token", 2 "invalid token payload"
from bytes import Bytes, balloc, eqlit, find_byte, parse_pos_int
from sha256 import Sha256
from jsonscan import JsonScan

comptime JWT_OK = 0
comptime JWT_INVALID = 1
comptime JWT_PAYLOAD = 2

comptime MAX_TOKEN = 4096


def b64url_val(c: UInt8) -> Int:
    if c >= 0x41 and c <= 0x5A:
        return Int(c - 0x41)
    if c >= 0x61 and c <= 0x7A:
        return Int(c - 0x61) + 26
    if c >= 0x30 and c <= 0x39:
        return Int(c - 0x30) + 52
    if c == 0x2D:
        return 62
    if c == 0x5F:
        return 63
    return -1


# Decode base64url (padding optional) into out; returns byte count or -1.
def b64url_decode(src: Bytes, n: Int, dst: Bytes) -> Int:
    var m = n
    while m > 0 and src[unsafe_offset=m - 1] == 0x3D:
        m -= 1
    var o = 0
    var acc: UInt32 = 0
    var bits = 0
    for i in range(m):
        var v = b64url_val(src[unsafe_offset=i])
        if v < 0:
            return -1
        acc = (acc << 6) | UInt32(v)
        bits += 6
        if bits >= 8:
            bits -= 8
            dst[unsafe_offset=o] = UInt8((acc >> UInt32(bits)) & 0xFF)
            o += 1
    if m % 4 == 1:
        return -1
    return o


struct Jwt:
    var sha: Sha256
    var hdr: Bytes     # decoded header
    var pay: Bytes     # decoded payload (username points into this until the next verify)
    var sig: Bytes
    var mac: Bytes
    var user_id: Int64
    var name_ptr: Bytes
    var name_len: Int

    def __init__(out self, secret: Bytes, secret_len: Int):
        self.sha = Sha256()
        self.sha.set_key(secret, secret_len)
        self.hdr = balloc(MAX_TOKEN)
        self.pay = balloc(MAX_TOKEN)
        self.sig = balloc(MAX_TOKEN)
        self.mac = balloc(32)
        self.user_id = 0
        self.name_ptr = self.pay
        self.name_len = 0

    def verify(mut self, tok: Bytes, n: Int, now: Int64) -> Int:
        if n < 3 or n > MAX_TOKEN:
            return JWT_INVALID
        var d1 = find_byte(tok, n, 0, 0x2E)
        if d1 <= 0:
            return JWT_INVALID
        var d2 = find_byte(tok, n, d1 + 1, 0x2E)
        if d2 <= d1 + 1 or d2 >= n - 1 or find_byte(tok, n, d2 + 1, 0x2E) >= 0:
            return JWT_INVALID
        # Signature first: cheapest rejection of garbage, and never leak header checks on unsigned tokens.
        var sn = b64url_decode(tok.unsafe_offset(d2 + 1), n - d2 - 1, self.sig)
        if sn != 32:
            return JWT_INVALID
        self.sha.hmac(tok, d2, self.mac)
        var diff: UInt8 = 0
        for i in range(32):
            diff |= self.mac[unsafe_offset=i] ^ self.sig[unsafe_offset=i]
        if diff != 0:
            return JWT_INVALID
        # Header: must be JSON with "alg":"HS256".
        var hn = b64url_decode(tok, d1, self.hdr)
        if hn < 0:
            return JWT_INVALID
        var hs = JsonScan(self.hdr, hn)
        if not hs.find_key("alg"):
            return JWT_INVALID
        if not hs.value_is_string():
            return JWT_INVALID
        var alg_len = hs.string_raw_len()
        if not eqlit(hs.string_raw_ptr(), alg_len, "HS256"):
            return JWT_INVALID
        # Payload: exp (number, > now), sub (positive integer string), username (string).
        var pn = b64url_decode(tok.unsafe_offset(d1 + 1), d2 - d1 - 1, self.pay)
        if pn < 0:
            return JWT_INVALID
        var ps = JsonScan(self.pay, pn)
        if not ps.find_key("exp"):
            return JWT_INVALID
        var exp = ps.number_int()
        if exp < 0 or exp <= now:
            return JWT_INVALID
        var ss = JsonScan(self.pay, pn)
        if not ss.find_key("sub") or not ss.value_is_string():
            return JWT_PAYLOAD
        var uid = parse_pos_int(ss.string_raw_ptr(), ss.string_raw_len())
        if uid <= 0:
            return JWT_PAYLOAD
        var us = JsonScan(self.pay, pn)
        if not us.find_key("username") or not us.value_is_string():
            return JWT_PAYLOAD
        # Username is used as-is; take the unescaped form.
        var ulen = us.string_unescape(self.sig, MAX_TOKEN)
        if ulen < 0:
            return JWT_PAYLOAD
        self.user_id = uid
        self.name_ptr = self.sig
        self.name_len = ulen
        return JWT_OK
