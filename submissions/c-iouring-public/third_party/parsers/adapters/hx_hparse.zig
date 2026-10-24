// parser/cmp/hx_hparse.zig (sha256 f183e8c7d6f6), developed separately for this entry.
// hparse (vendor/hparse, nikneym/hparse 4fb3d83) behind picohttpparser's phr_parse_request signature.
const std = @import("std");
const hparse = @import("hparse");

const PhrHeader = extern struct { name: [*]const u8, name_len: usize, value: [*]const u8, value_len: usize };
const names = [_][]const u8{ "", "GET", "POST", "HEAD", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH" };

/// returns bytes consumed, -1 invalid, -2 incomplete (as pico)
export fn hp_parse_request(buf: [*]const u8, len: usize, method: *[*]const u8, method_len: *usize, path: *[*]const u8,
    path_len: *usize, minor: *c_int, headers: [*]PhrHeader, num_headers: *usize, last_len: usize) c_int {
    _ = last_len;
    var m: hparse.Method = .unknown;
    var p: ?[]const u8 = null;
    var v: hparse.Version = .@"1.0";
    var hs: [48]hparse.Header = undefined;
    var n: usize = 0;
    const max = @min(num_headers.*, 48);
    const used = hparse.parseRequest(buf[0..len], &m, &p, &v, hs[0..max], &n) catch |e| return switch (e) {
        error.Incomplete => -2,
        error.Invalid => -1,
    };
    const name = names[@intFromEnum(m)];   // hparse returns the method as an enum, not a slice of buf
    method.* = name.ptr;
    method_len.* = name.len;
    path.* = p.?.ptr;
    path_len.* = p.?.len;
    minor.* = if (v == .@"1.1") 1 else 0;
    for (hs[0..n], 0..) |h, i| headers[i] = .{ .name = h.key.ptr, .name_len = h.key.len, .value = h.value.ptr, .value_len = h.value.len };
    num_headers.* = n;
    return @intCast(used);
}
