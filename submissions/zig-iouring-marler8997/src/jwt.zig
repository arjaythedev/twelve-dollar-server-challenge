//! HS256 JWT verification. The HMAC state keyed with the secret is computed
//! once and copied per token; every token's signature is still checked.

pub const HmacSha256 = std.crypto.auth.hmac.sha2.HmacSha256;

pub const Claims = struct {
    user_id: i64,
    username: []const u8,
};

pub const Error = error{
    /// Bad signature, expired, not HS256, or malformed: "invalid or expired token".
    Invalid,
    /// Valid token, unusable `sub` or `username`: "invalid token payload".
    BadPayload,
};

/// The longest payload `verify` accepts, after base64 decoding.
pub const max_payload = 1024;

/// The payload's strings are decoded into `strings`, which the returned
/// username points into.
pub fn verify(keyed: *const HmacSha256, token: []const u8, now_s: i64, strings: *[max_payload]u8) Error!Claims {
    const dot1 = std.mem.indexOfScalar(u8, token, '.') orelse return error.Invalid;
    const dot2 = std.mem.indexOfScalarPos(u8, token, dot1 + 1, '.') orelse return error.Invalid;
    const header_b64 = token[0..dot1];
    const payload_b64 = token[dot1 + 1 .. dot2];
    const sig_b64 = token[dot2 + 1 ..];

    var mac: [HmacSha256.mac_length]u8 = undefined;
    var h = keyed.*;
    h.update(token[0..dot2]);
    h.final(&mac);
    var expected: [base64.calcSize(HmacSha256.mac_length)]u8 = undefined;
    _ = base64.encode(&expected, &mac);
    if (sig_b64.len != expected.len) return error.Invalid;
    if (!std.crypto.timing_safe.eql([expected.len]u8, sig_b64[0..expected.len].*, expected)) return error.Invalid;

    if (!std.mem.eql(u8, header_b64, canonical_header)) {
        var buf: [256]u8 = undefined;
        const header_json = decode(header_b64, &buf) orelse return error.Invalid;
        const header = json.parse(Header, header_json, strings) catch return error.Invalid;
        if (!std.mem.eql(u8, header.alg orelse return error.Invalid, "HS256")) return error.Invalid;
    }

    var payload_buf: [max_payload]u8 = undefined;
    const payload_json = decode(payload_b64, &payload_buf) orelse return error.Invalid;
    const payload = json.parse(Payload, payload_json, strings) catch return error.Invalid;
    const exp = payload.exp orelse return error.Invalid;
    if (!(exp > @as(f64, @floatFromInt(now_s)))) return error.Invalid;
    const user_id = parsePositive(payload.sub orelse return error.BadPayload) orelse return error.BadPayload;
    const username = payload.username orelse return error.BadPayload;
    return .{ .user_id = user_id, .username = username };
}

const Header = struct { alg: ?[]const u8 };
const Payload = struct { sub: ?[]const u8, username: ?[]const u8, exp: ?f64 };

/// base64url of `{"alg":"HS256","typ":"JWT"}`, what every HS256 signer sends.
const canonical_header = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

const base64 = std.base64.url_safe_no_pad.Encoder;

fn decode(src: []const u8, buf: []u8) ?[]u8 {
    const d = std.base64.url_safe_no_pad.Decoder;
    const len = d.calcSizeForSlice(src) catch return null;
    if (len > buf.len) return null;
    d.decode(buf[0..len], src) catch return null;
    return buf[0..len];
}

/// A positive decimal integer without sign, as used for ids.
pub fn parsePositive(s: []const u8) ?i64 {
    if (s.len == 0 or s.len > 18) return null;
    var v: i64 = 0;
    for (s) |c| {
        if (c < '0' or c > '9') return null;
        v = v * 10 + (c - '0');
    }
    return if (v > 0) v else null;
}

const std = @import("std");
const json = @import("json.zig");
