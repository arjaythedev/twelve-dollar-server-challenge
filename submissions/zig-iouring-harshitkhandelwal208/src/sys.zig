//! Thin helpers over raw Linux syscalls for high-performance memory and process control.
const std = @import("std");
const linux = std.os.linux;

pub const Errno = linux.E;

pub inline fn errnoOf(rc: usize) Errno {
    return linux.errno(rc);
}

pub fn nowUnix() i64 {
    var ts: linux.timespec = undefined;
    _ = linux.clock_gettime(.REALTIME_COARSE, &ts);
    return ts.sec;
}

pub fn monoSecs() u32 {
    var ts: linux.timespec = undefined;
    _ = linux.clock_gettime(.MONOTONIC_COARSE, &ts);
    return @truncate(@as(u64, @bitCast(ts.sec)));
}

pub fn mapAnon(len: usize) ![]align(4096) u8 {
    const rc = linux.mmap(null, len, .{ .READ = true, .WRITE = true }, .{ .TYPE = .PRIVATE, .ANONYMOUS = true }, -1, 0);
    if (errnoOf(rc) != .SUCCESS) return error.OutOfMemory;
    const p: [*]align(4096) u8 = @ptrFromInt(rc);
    return p[0..len];
}

pub fn unmap(mem: []align(4096) u8) void {
    _ = linux.munmap(mem.ptr, mem.len);
}

pub fn madviseHuge(mem: []const u8) void {
    _ = linux.madvise(@constCast(@ptrCast(mem.ptr)), mem.len, linux.MADV.HUGEPAGE);
}

/// Fault one byte per base page during startup.  Call this only for fixed
/// reactor mappings; large response slabs deliberately remain demand-paged.
pub fn prefault(mem: []u8) void {
    var off: usize = 0;
    while (off < mem.len) : (off += 4096) mem[off] = 0;
}

pub fn unshareFiles() void {
    _ = linux.syscall1(.unshare, 0x00000400); // CLONE_FILES
}
