//! Strict JSON parsing straight into a schema struct, and the string escaping
//! the responses need. No allocation.

pub const Error = error{Malformed};

/// Parses `input` as one JSON document into `T`, a struct whose fields are
/// all `?[]const u8` (strings, decoded) or `?f64` (numbers). If the document
/// is an object, each field takes the last member of its name; a field is
/// null when that member is missing or holds another type. Other members, and
/// documents that aren't objects, are validated and ignored: only invalid
/// JSON is an error. Decoded strings are written to `strings`, which must be
/// at least as long as `input` (decoding never makes text longer).
pub fn parse(comptime T: type, input: []const u8, strings: []u8) Error!T {
    std.debug.assert(strings.len >= input.len);
    const info = @typeInfo(T).@"struct";
    var result: T = undefined;
    inline for (info.field_names) |name| @field(result, name) = null;
    var p: Parser = .{ .s = input, .out = strings };
    p.ws();
    if (p.peek() == '{') {
        p.i += 1;
        p.ws();
        if (p.peek() == '}') {
            p.i += 1;
        } else while (true) {
            p.ws();
            if (p.peek() != '"') return error.Malformed;
            // Keys are compared decoded, then their bytes are reused.
            const mark = p.n;
            const key = try p.string();
            p.n = mark;
            p.ws();
            if (p.peek() != ':') return error.Malformed;
            p.i += 1;
            p.ws();
            member: {
                inline for (info.field_names, info.field_types) |name, F| {
                    if (std.mem.eql(u8, key, name)) {
                        @field(result, name) = try p.typed(@typeInfo(F).optional.child);
                        break :member;
                    }
                }
                try p.skip(0);
            }
            p.ws();
            switch (p.peek()) {
                ',' => p.i += 1,
                '}' => {
                    p.i += 1;
                    break;
                },
                else => return error.Malformed,
            }
        }
    } else {
        try p.skip(0);
    }
    p.ws();
    if (p.i != input.len) return error.Malformed;
    return result;
}

const max_depth = 512;

const Parser = struct {
    s: []const u8,
    i: usize = 0,
    /// Decoded strings go to `out[n..]`.
    out: []u8,
    n: usize = 0,

    fn peek(p: *Parser) u8 {
        return if (p.i < p.s.len) p.s[p.i] else 0;
    }

    fn ws(p: *Parser) void {
        while (p.i < p.s.len) : (p.i += 1) switch (p.s[p.i]) {
            ' ', '\t', '\n', '\r' => {},
            else => return,
        };
    }

    /// Parses a value as `V`, or validates and skips it (returning null) if
    /// it is of another type.
    fn typed(p: *Parser, comptime V: type) Error!?V {
        switch (V) {
            []const u8 => if (p.peek() == '"') return try p.string(),
            f64 => switch (p.peek()) {
                '-', '0'...'9' => {
                    const start = p.i;
                    try p.number();
                    return std.fmt.parseFloat(f64, p.s[start..p.i]) catch error.Malformed;
                },
                else => {},
            },
            else => @compileError("unsupported JSON field type " ++ @typeName(V)),
        }
        try p.skip(0);
        return null;
    }

    fn skip(p: *Parser, depth: u32) Error!void {
        if (depth > max_depth) return error.Malformed;
        switch (p.peek()) {
            '"' => {
                const mark = p.n;
                _ = try p.string();
                p.n = mark;
            },
            '-', '0'...'9' => try p.number(),
            '{' => {
                p.i += 1;
                p.ws();
                if (p.peek() == '}') {
                    p.i += 1;
                    return;
                }
                while (true) {
                    p.ws();
                    if (p.peek() != '"') return error.Malformed;
                    try p.skip(depth + 1);
                    p.ws();
                    if (p.peek() != ':') return error.Malformed;
                    p.i += 1;
                    p.ws();
                    try p.skip(depth + 1);
                    p.ws();
                    switch (p.peek()) {
                        ',' => p.i += 1,
                        '}' => {
                            p.i += 1;
                            return;
                        },
                        else => return error.Malformed,
                    }
                }
            },
            '[' => {
                p.i += 1;
                p.ws();
                if (p.peek() == ']') {
                    p.i += 1;
                    return;
                }
                while (true) {
                    p.ws();
                    try p.skip(depth + 1);
                    p.ws();
                    switch (p.peek()) {
                        ',' => p.i += 1,
                        ']' => {
                            p.i += 1;
                            return;
                        },
                        else => return error.Malformed,
                    }
                }
            },
            't' => try p.literal("true"),
            'f' => try p.literal("false"),
            'n' => try p.literal("null"),
            else => return error.Malformed,
        }
    }

    fn literal(p: *Parser, comptime word: []const u8) Error!void {
        if (!std.mem.startsWith(u8, p.s[p.i..], word)) return error.Malformed;
        p.i += word.len;
    }

    /// Decodes a string to `out`; `p.peek()` must be the opening quote.
    fn string(p: *Parser) Error![]const u8 {
        p.i += 1;
        const start = p.n;
        while (true) {
            const run_start = p.i;
            while (p.i < p.s.len) : (p.i += 1) switch (p.s[p.i]) {
                '"', '\\', 0...0x1f => break,
                else => {},
            };
            const run = p.s[run_start..p.i];
            @memcpy(p.out[p.n..][0..run.len], run);
            p.n += run.len;
            switch (p.peek()) {
                '"' => {
                    p.i += 1;
                    return p.out[start..p.n];
                },
                '\\' => try p.escape(),
                else => return error.Malformed, // control character or end of input
            }
        }
    }

    /// Decodes the escape at `p.i`.
    fn escape(p: *Parser) Error!void {
        if (p.i + 1 >= p.s.len) return error.Malformed;
        const byte: u8 = switch (p.s[p.i + 1]) {
            '"', '\\', '/' => |c| c,
            'b' => 0x08,
            'f' => 0x0c,
            'n' => '\n',
            'r' => '\r',
            't' => '\t',
            'u' => {
                var cp = try p.hex4(p.i + 2);
                p.i += 6;
                if (cp >= 0xd800 and cp < 0xdc00) {
                    // A high surrogate combines with a low one that follows.
                    const lo = if (std.mem.startsWith(u8, p.s[p.i..], "\\u")) try p.hex4(p.i + 2) else 0;
                    if (lo >= 0xdc00 and lo < 0xe000) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p.i += 6;
                    } else cp = 0xfffd;
                } else if (cp >= 0xdc00 and cp < 0xe000) cp = 0xfffd;
                // Never longer than the escape: 6 bytes give at most 3, 12 give 4.
                p.n += std.unicode.utf8Encode(cp, p.out[p.n..][0..4]) catch unreachable;
                return;
            },
            else => return error.Malformed,
        };
        p.out[p.n] = byte;
        p.n += 1;
        p.i += 2;
    }

    fn hex4(p: *Parser, at: usize) Error!u21 {
        if (at + 4 > p.s.len) return error.Malformed;
        var v: u21 = 0;
        for (p.s[at..][0..4]) |c| v = v * 16 + (std.fmt.charToDigit(c, 16) catch return error.Malformed);
        return v;
    }

    fn number(p: *Parser) Error!void {
        if (p.peek() == '-') p.i += 1;
        switch (p.peek()) {
            '0' => p.i += 1,
            '1'...'9' => p.digits(),
            else => return error.Malformed,
        }
        if (p.peek() == '.') {
            p.i += 1;
            if (!std.ascii.isDigit(p.peek())) return error.Malformed;
            p.digits();
        }
        if (p.peek() == 'e' or p.peek() == 'E') {
            p.i += 1;
            if (p.peek() == '+' or p.peek() == '-') p.i += 1;
            if (!std.ascii.isDigit(p.peek())) return error.Malformed;
            p.digits();
        }
    }

    fn digits(p: *Parser) void {
        while (std.ascii.isDigit(p.peek())) p.i += 1;
    }
};

test parse {
    const S = struct { body: ?[]const u8, n: ?f64 };
    var strings: [256]u8 = undefined;

    // Escapes decode, the last duplicate wins (escaped keys count), other
    // members are skipped.
    var r = try parse(S,
        \\ {"x":[1,{"y":null}],"bo\u0064y":"old","body":"a\"\\\/\n\u00e9\ud83d\ude00\ud83dx","n":-1.5e2}
    , &strings);
    try std.testing.expectEqualStrings("a\"\\/\n\u{e9}\u{1F600}\u{FFFD}x", r.body.?);
    try std.testing.expectEqual(-150, r.n.?);

    // Valid JSON of the wrong shape leaves fields null instead of failing:
    // the API answers "body is required", not "malformed JSON body".
    r = try parse(S,
        \\{"body":1,"n":"1"}
    , &strings);
    try std.testing.expect(r.body == null and r.n == null);
    r = try parse(S,
        \\["body"]
    , &strings);
    try std.testing.expect(r.body == null);

    const malformed = [_][]const u8{
        "",
        "{",
        \\{"body":"x",}
        ,
        \\{"body":"x"} x
        ,
        \\{"body":"\x"}
        ,
        \\{"body":"\u12"}
        ,
        "{\"body\":\"\x01\"}",
        \\{"body":"x","n":01}
        ,
        \\{"body":"x","n":tru}
        ,
    };
    for (malformed) |input| try std.testing.expectError(error.Malformed, parse(S, input, &strings));
}

/// Writes `s` as the contents of a JSON string (without quotes).
pub fn writeEscaped(w: *std.Io.Writer, s: []const u8) std.Io.Writer.Error!void {
    var start: usize = 0;
    while (true) {
        const i = start + plainLen(s[start..]);
        try w.writeAll(s[start..i]);
        if (i == s.len) return;
        const c = s[i];
        switch (c) {
            '"' => try w.writeAll("\\\""),
            '\\' => try w.writeAll("\\\\"),
            '\n' => try w.writeAll("\\n"),
            '\r' => try w.writeAll("\\r"),
            '\t' => try w.writeAll("\\t"),
            else => try w.print("\\u{x:0>4}", .{c}),
        }
        start = i + 1;
    }
}

/// The length of the start of `s` that a JSON string holds as is: up to the
/// first control character, '"' or '\\'.
fn plainLen(s: []const u8) usize {
    const n = comptime std.simd.suggestVectorLength(u8) orelse 16;
    const V = @Vector(n, u8);
    var i: usize = 0;
    while (i + n <= s.len) : (i += n) {
        const v: V = s[i..][0..n].*;
        const special = @intFromBool(v < @as(V, @splat(0x20))) |
            @intFromBool(v == @as(V, @splat('"'))) |
            @intFromBool(v == @as(V, @splat('\\')));
        if (std.simd.firstTrue(special != @as(@Vector(n, u1), @splat(0)))) |j| return i + j;
    }
    while (i < s.len) : (i += 1) switch (s[i]) {
        0...0x1f, '"', '\\' => return i,
        else => {},
    };
    return i;
}

test writeEscaped {
    // Every byte value at every offset of a string longer than two vectors,
    // checked against a byte-at-a-time escape.
    var input: [80]u8 = undefined;
    var out_buf: [80 * 6]u8 = undefined;
    var want_buf: [80 * 6]u8 = undefined;
    for (0..256) |c| for (0..input.len) |at| {
        @memset(&input, 'a');
        input[at] = @intCast(c);
        input[input.len - 1 - at / 2] = '"';
        var out: std.Io.Writer = .fixed(&out_buf);
        try writeEscaped(&out, &input);
        var want: std.Io.Writer = .fixed(&want_buf);
        for (input) |b| switch (b) {
            '"' => try want.writeAll("\\\""),
            '\\' => try want.writeAll("\\\\"),
            '\n' => try want.writeAll("\\n"),
            '\r' => try want.writeAll("\\r"),
            '\t' => try want.writeAll("\\t"),
            0...0x08, 0x0b, 0x0c, 0x0e...0x1f => try want.print("\\u{x:0>4}", .{b}),
            else => try want.writeByte(b),
        };
        try std.testing.expectEqualStrings(want.buffered(), out.buffered());
    };
}

/// Trims what JavaScript's `String.prototype.trim` trims.
pub fn trim(s: []const u8) []const u8 {
    var start: usize = 0;
    var end: usize = s.len;
    while (start < end) {
        const len = spaceLenAt(s[start..end]);
        if (len == 0) break;
        start += len;
    }
    while (start < end) {
        const len = spaceLenBefore(s[start..end]);
        if (len == 0) break;
        end -= len;
    }
    return s[start..end];
}

fn isSpace(cp: u21) bool {
    return switch (cp) {
        '\t', '\n', 0x0b, 0x0c, '\r', ' ', 0xa0, 0x1680, 0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff => true,
        else => false,
    };
}

fn spaceLenAt(s: []const u8) usize {
    const len = std.unicode.utf8ByteSequenceLength(s[0]) catch return 0;
    if (len > s.len) return 0;
    const cp = std.unicode.utf8Decode(s[0..len]) catch return 0;
    return if (isSpace(cp)) len else 0;
}

fn spaceLenBefore(s: []const u8) usize {
    var start = s.len - 1;
    while (start > 0 and s.len - start < 4 and s[start] & 0xc0 == 0x80) start -= 1;
    const len = spaceLenAt(s[start..]);
    return if (start + len == s.len) len else 0;
}

/// Counts code points, which is what SQLite's `length()` (and the schema's
/// CHECK) counts.
pub fn codePointCount(s: []const u8) usize {
    var n: usize = 0;
    for (s) |c| n += @intFromBool(c & 0xc0 != 0x80);
    return n;
}

const std = @import("std");
