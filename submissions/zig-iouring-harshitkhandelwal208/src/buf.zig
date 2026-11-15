const std = @import("std");

pub const gpa = std.heap.c_allocator;

pub const digits: *const [200]u8 =
    "0001020304050607080910111213141516171819" ++
    "2021222324252627282930313233343536373839" ++
    "4041424344454647484950515253545556575859" ++
    "6061626364656667686970717273747576777879" ++
    "8081828384858687888990919293949596979899";

pub const Buf = struct {
    ptr: [*]u8 = undefined,
    len: usize = 0,
    cap: usize = 0,

    pub fn init(capacity: usize) !Buf {
        const p = try gpa.alloc(u8, capacity);
        return .{ .ptr = p.ptr, .len = 0, .cap = capacity };
    }

    pub fn deinit(self: *Buf) void {
        if (self.cap > 0) gpa.free(self.ptr[0..self.cap]);
        self.* = .{};
    }

    pub fn reset(self: *Buf) void {
        self.len = 0;
    }

    pub fn ensure(self: *Buf, extra: usize) !void {
        if (self.len + extra <= self.cap) return;
        var new_cap = if (self.cap == 0) 4096 else self.cap * 2;
        while (new_cap < self.len + extra) new_cap *= 2;
        const new_mem = try gpa.realloc(self.ptr[0..self.cap], new_cap);
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
        self.writeAll(b[i..24]);
    }

    pub fn writeJsonString(self: *Buf, s: []const u8) void {
        self.ensure(s.len * 2 + 2) catch @panic("OOM");
        for (s) |ch| {
            switch (ch) {
                '"' => self.writeAll("\\\""),
                '\\' => self.writeAll("\\\\"),
                '\n' => self.writeAll("\\n"),
                '\r' => self.writeAll("\\r"),
                '\t' => self.writeAll("\\t"),
                else => {
                    if (ch < 0x20) {
                        var esc: [6]u8 = undefined;
                        _ = std.fmt.bufPrint(&esc, "\\u00{x:0>2}", .{ch}) catch unreachable;
                        self.writeAll(&esc);
                    } else {
                        self.writeByte(ch);
                    }
                },
            }
        }
    }

    pub fn slice(self: *const Buf) []const u8 {
        return self.ptr[0..self.len];
    }
};
