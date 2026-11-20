const std = @import("std");

pub const gpa = std.heap.c_allocator;

pub const digits: *const [200]u8 =
    "0001020304050607080910111213141516171819" ++
    "2021222324252627282930313233343536373839" ++
    "4041424344454647484950515253545556575859" ++
    "6061626364656667686970717273747576777879" ++
    "8081828384858687888990919293949596979899";

pub inline fn digitsU64(val: u64) usize {
    if (val < 10) return 1;
    if (val < 100) return 2;
    if (val < 1000) return 3;
    if (val < 10000) return 4;
    if (val < 100000) return 5;
    if (val < 1000000) return 6;
    if (val < 10000000) return 7;
    if (val < 100000000) return 8;
    if (val < 1000000000) return 9;
    if (val < 10000000000) return 10;
    var v = val;
    var len: usize = 0;
    while (v > 0) : (v /= 10) len += 1;
    return len;
}

pub inline fn escapedLen(s: []const u8) usize {
    var extra: usize = 0;
    var i: usize = 0;

    const V = @Vector(16, u8);
    const quote_v: V = @splat('"');
    const slash_v: V = @splat('\\');
    const space_v: V = @splat(0x20);

    while (i + 16 <= s.len) {
        const chunk: V = s[i..][0..16].*;
        const has_quote = chunk == quote_v;
        const has_slash = chunk == slash_v;
        const has_ctrl = chunk < space_v;
        const needs_escape = has_quote | has_slash | has_ctrl;

        if (!@reduce(.Or, needs_escape)) {
            i += 16;
        } else {
            const end = i + 16;
            while (i < end) : (i += 1) {
                const ch = s[i];
                if (ch == '"' or ch == '\\') {
                    extra += 1;
                } else if (ch < 0x20) {
                    extra += if (ch == '\n' or ch == '\r' or ch == '\t') 1 else 5;
                }
            }
        }
    }

    while (i < s.len) : (i += 1) {
        const ch = s[i];
        if (ch == '"' or ch == '\\') {
            extra += 1;
        } else if (ch < 0x20) {
            extra += if (ch == '\n' or ch == '\r' or ch == '\t') 1 else 5;
        }
    }
    return s.len + extra;
}

pub inline fn writeU64Direct(buf: []u8, val: u64) usize {
    if (val < 10) {
        buf[0] = '0' + @as(u8, @intCast(val));
        return 1;
    }
    if (val < 100) {
        const off: usize = @intCast(val * 2);
        buf[0..2].* = digits[off..][0..2].*;
        return 2;
    }
    var v = val;
    var b: [24]u8 = undefined;
    var i: usize = 24;
    while (v >= 100) {
        const r: usize = @intCast(v % 100);
        v /= 100;
        i -= 2;
        b[i] = digits[r * 2];
        b[i + 1] = digits[r * 2 + 1];
    }
    if (v >= 10) {
        const r: usize = @intCast(v);
        i -= 2;
        b[i] = digits[r * 2];
        b[i + 1] = digits[r * 2 + 1];
    } else {
        i -= 1;
        b[i] = '0' + @as(u8, @truncate(v));
    }
    const len = 24 - i;
    @memcpy(buf[0..len], b[i..24]);
    return len;
}

inline fn cleanJsonChunk(comptime width: usize, bytes: @Vector(width, u8)) bool {
    const V = @Vector(width, u8);
    return !@reduce(.Or, (bytes == @as(V, @splat('"'))) | (bytes == @as(V, @splat('\\'))) | (bytes < @as(V, @splat(0x20))));
}

pub inline fn writeJsonStringDirect(dest: [*]u8, s: []const u8) usize {
    var i: usize = 0;
    if (s.len >= 16) {
        const last = s.len - 16;
        while (i < last and cleanJsonChunk(16, s[i..][0..16].*)) : (i += 16) {
            dest[i..][0..16].* = s[i..][0..16].*;
        }
        // An overlapping final vector stays inside the input and output.
        if (i >= last and cleanJsonChunk(16, s[last..][0..16].*)) {
            dest[last..][0..16].* = s[last..][0..16].*;
            return s.len;
        }
    } else if (s.len >= 8) {
        const last = s.len - 8;
        if (cleanJsonChunk(8, s[0..8].*) and cleanJsonChunk(8, s[last..][0..8].*)) {
            dest[0..8].* = s[0..8].*;
            dest[last..][0..8].* = s[last..][0..8].*;
            return s.len;
        }
    }

    var d = i;
    const hex = "0123456789abcdef";
    while (i < s.len) : (i += 1) {
        const ch = s[i];
        if (ch == '"' or ch == '\\' or ch < 0x20) {
            switch (ch) {
                '"' => {
                    @memcpy(dest[d..][0..2], "\\\"");
                    d += 2;
                },
                '\\' => {
                    @memcpy(dest[d..][0..2], "\\\\");
                    d += 2;
                },
                '\n' => {
                    @memcpy(dest[d..][0..2], "\\n");
                    d += 2;
                },
                '\r' => {
                    @memcpy(dest[d..][0..2], "\\r");
                    d += 2;
                },
                '\t' => {
                    @memcpy(dest[d..][0..2], "\\t");
                    d += 2;
                },
                else => {
                    dest[d] = '\\';
                    dest[d + 1] = 'u';
                    dest[d + 2] = '0';
                    dest[d + 3] = '0';
                    dest[d + 4] = hex[ch >> 4];
                    dest[d + 5] = hex[ch & 0xf];
                    d += 6;
                },
            }
        } else {
            dest[d] = ch;
            d += 1;
        }
    }
    return d;
}

pub const Buf = struct {
    ptr: [*]u8 = undefined,
    len: usize = 0,
    cap: usize = 0,
    borrowed: bool = false,

    pub fn wrap(storage: []u8) Buf {
        return .{ .ptr = storage.ptr, .cap = storage.len, .borrowed = true };
    }

    pub fn init(capacity: usize) !Buf {
        const p = try gpa.alloc(u8, capacity);
        return .{ .ptr = p.ptr, .len = 0, .cap = capacity };
    }

    pub fn deinit(self: *Buf) void {
        if (!self.borrowed and self.cap > 0) gpa.free(self.ptr[0..self.cap]);
        self.* = .{};
    }

    pub fn reset(self: *Buf) void {
        self.len = 0;
    }

    pub fn ensure(self: *Buf, extra: usize) !void {
        if (self.len + extra <= self.cap) return;
        var new_cap = if (self.cap == 0) 4096 else self.cap * 2;
        while (new_cap < self.len + extra) new_cap *= 2;
        const new_mem = if (self.borrowed) blk: {
            const mem = try gpa.alloc(u8, new_cap);
            @memcpy(mem[0..self.len], self.slice());
            self.borrowed = false;
            break :blk mem;
        } else try gpa.realloc(self.ptr[0..self.cap], new_cap);
        self.ptr = new_mem.ptr;
        self.cap = new_cap;
    }

    pub fn writeAll(self: *Buf, bytes: []const u8) void {
        self.ensure(bytes.len) catch @panic("OOM");
        @memcpy(self.ptr[self.len..][0..bytes.len], bytes);
        self.len += bytes.len;
    }

    pub fn writeByte(self: *Buf, b: u8) void {
        self.ensure(1) catch @panic("OOM");
        self.ptr[self.len] = b;
        self.len += 1;
    }

    pub fn writeU64(self: *Buf, val: u64) void {
        self.ensure(24) catch @panic("OOM");
        const written = writeU64Direct(self.ptr[self.len..][0..24], val);
        self.len += written;
    }

    pub fn writeJsonString(self: *Buf, s: []const u8) void {
        self.ensure(s.len * 6 + 2) catch @panic("OOM");
        self.len += writeJsonStringDirect(self.ptr + self.len, s);
    }

    pub fn slice(self: *const Buf) []const u8 {
        return self.ptr[0..self.len];
    }
};

test "writeU64Direct and writeJsonString" {
    var b = try Buf.init(1024);
    defer b.deinit();

    b.writeU64(1234567890);
    try std.testing.expectEqualStrings("1234567890", b.slice());

    b.reset();
    b.writeJsonString("hello \"world\"\n");
    try std.testing.expectEqualStrings("hello \\\"world\\\"\\n", b.slice());
}

test "JSON string copies and escapes at vector boundaries" {
    var input: [65]u8 = @splat('a');
    var output: [130]u8 = undefined;
    for (0..input.len + 1) |len| {
        try std.testing.expectEqual(len, writeJsonStringDirect(&output, input[0..len]));
        try std.testing.expectEqualSlices(u8, input[0..len], output[0..len]);
        for (0..len) |pos| {
            input[pos] = '\n';
            try std.testing.expectEqual(len + 1, writeJsonStringDirect(&output, input[0..len]));
            try std.testing.expectEqualSlices(u8, input[0..pos], output[0..pos]);
            try std.testing.expectEqualStrings("\\n", output[pos..][0..2]);
            try std.testing.expectEqualSlices(u8, input[pos + 1 .. len], output[pos + 2 .. len + 1]);
            input[pos] = 'a';
        }
    }
}
