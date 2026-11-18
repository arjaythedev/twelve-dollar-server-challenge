const std = @import("std");
const gpa = @import("buf.zig").gpa;

pub const Scratch = struct {
    mem: []u8,
    off: usize = 0,

    pub fn init(size: usize) !Scratch {
        const mem = try gpa.alloc(u8, size);
        return .{ .mem = mem, .off = 0 };
    }

    pub fn deinit(self: *Scratch) void {
        gpa.free(self.mem);
    }

    pub fn reset(self: *Scratch) void {
        self.off = 0;
    }

    pub fn alloc(self: *Scratch, len: usize) ![]u8 {
        if (self.off + len > self.mem.len) return error.OutOfMemory;
        const res = self.mem[self.off .. self.off + len];
        self.off += len;
        return res;
    }
};
