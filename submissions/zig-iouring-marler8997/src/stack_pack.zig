//! A compact encoding for parked fiber stacks. Each aligned 8-byte word is
//! stored as a 2-bit tag plus 0, 2, 4 or 8 bytes. A word costs only its tag
//! when it equals the same word of a `Base`, a stack parked earlier at the
//! same place: the same frames, return addresses and pointers into its own
//! slot. Without a base, zero words cost only their tag. Otherwise pointers
//! into the stack's own slot take 2 bytes and small integers 4.

/// Stack slots are at most this big, so a pointer into one is a u16 offset.
pub const max_slot_size = 64 * 1024;

const Tag = enum(u2) { base, slot_offset, u32, u64 };

/// What a stack is packed against: bytes that were at the same offsets in a
/// slot starting at `slot`, as long as the packed bytes.
pub const Base = struct {
    bytes: []const u8,
    slot: usize,

    /// No base: words are compared against zero.
    pub const none: Base = .{ .bytes = &.{}, .slot = 0 };

    /// Word `i` of the base as it would read in the slot at `slot`: a pointer
    /// into the base's slot points into `slot` instead.
    fn word(b: Base, shape: Shape, i: usize, slot: usize) u64 {
        if (b.bytes.len == 0) return 0;
        const w = std.mem.readInt(u64, b.bytes[shape.prefix + i * 8 ..][0..8], .little);
        return if (w -% b.slot < max_slot_size) slot + (w - b.slot) else w;
    }
};

/// The most `pack` writes for `len` bytes.
pub fn bound(len: usize) usize {
    return len / 32 + 1 + len;
}

/// Packs `bytes`, which live in the stack slot starting at address `slot`,
/// into `out`. Returns the length written.
pub fn pack(out: []u8, bytes: []const u8, slot: usize, base: Base) usize {
    std.debug.assert(base.bytes.len == 0 or base.bytes.len == bytes.len);
    const shape: Shape = .of(bytes.ptr, bytes.len);
    const tags = out[0..shape.tagsLen()];
    @memset(tags, 0);
    var n = tags.len;
    @memcpy(out[n..][0..shape.prefix], bytes[0..shape.prefix]);
    n += shape.prefix;
    const words: []align(1) const u64 = @ptrCast(bytes[shape.prefix..][0 .. shape.words * 8]);
    for (words, 0..) |w, i| {
        const tag: Tag = if (w == base.word(shape, i, slot))
            .base
        else if (w -% slot < max_slot_size)
            .slot_offset
        else if (w <= std.math.maxInt(u32))
            .u32
        else
            .u64;
        tags[i / 4] |= @as(u8, @backingInt(tag)) << @intCast(i % 4 * 2);
        switch (tag) {
            .base => {},
            .slot_offset => n += put(u16, out[n..], @intCast(w - slot)),
            .u32 => n += put(u32, out[n..], @intCast(w)),
            .u64 => n += put(u64, out[n..], w),
        }
    }
    @memcpy(out[n..][0..shape.suffix], bytes[bytes.len - shape.suffix ..]);
    return n + shape.suffix;
}

/// Restores what `pack` packed into `out` (the same address and length),
/// given the same `slot` and `base`. Returns the length of `in` it consumed.
pub fn unpack(in: []const u8, out: []u8, slot: usize, base: Base) usize {
    const shape: Shape = .of(out.ptr, out.len);
    const tags = in[0..shape.tagsLen()];
    var n = tags.len;
    @memcpy(out[0..shape.prefix], in[n..][0..shape.prefix]);
    n += shape.prefix;
    const words: []align(1) u64 = @ptrCast(out[shape.prefix..][0 .. shape.words * 8]);
    for (words, 0..) |*w, i| {
        const tag: Tag = @fromBackingInt(@as(u2, @truncate(tags[i / 4] >> @intCast(i % 4 * 2))));
        w.* = switch (tag) {
            .base => base.word(shape, i, slot),
            .slot_offset => slot + get(u16, in, &n),
            .u32 => get(u32, in, &n),
            .u64 => get(u64, in, &n),
        };
    }
    @memcpy(out[out.len - shape.suffix ..], in[n..][0..shape.suffix]);
    return n + shape.suffix;
}

/// Raw bytes before the first aligned word and after the last.
const Shape = struct {
    prefix: usize,
    words: usize,
    suffix: usize,

    fn of(ptr: [*]const u8, len: usize) Shape {
        const prefix = @min(len, (8 - @intFromPtr(ptr) % 8) % 8);
        const words = (len - prefix) / 8;
        return .{ .prefix = prefix, .words = words, .suffix = len - prefix - words * 8 };
    }

    fn tagsLen(s: Shape) usize {
        return (s.words + 3) / 4;
    }
};

fn put(comptime T: type, out: []u8, v: T) usize {
    std.mem.writeInt(T, out[0..@sizeOf(T)], v, .little);
    return @sizeOf(T);
}

fn get(comptime T: type, in: []const u8, n: *usize) T {
    defer n.* += @sizeOf(T);
    return std.mem.readInt(T, in[n.*..][0..@sizeOf(T)], .little);
}

test "round trip" {
    const slot: usize = 0x7000_0000_0000;
    const base_slot: usize = slot + 5 * max_slot_size;
    var words: [40]u64 = undefined;
    var base_words: [40]u64 = undefined;
    for (&words, &base_words, 0..) |*w, *b, i| {
        w.* = switch (i % 5) {
            0 => 0,
            1 => slot + i * 24, // into the slot
            2 => i * 1000, // small
            3 => base_slot + i, // into the base's slot
            else => slot + max_slot_size, // just past the slot: not an offset
        };
        b.* = switch (i % 3) {
            0 => w.*, // the same
            1 => w.* -% slot +% base_slot, // the same once relocated, if into the slot
            else => w.* ^ 0x10, // different
        };
    }
    const bytes = std.mem.sliceAsBytes(&words);
    const base_bytes = std.mem.sliceAsBytes(&base_words);
    // Every alignment and length, so prefixes, suffixes and partial tag bytes all occur.
    for (0..9) |start| for (start..bytes.len) |end| {
        const src = bytes[start..end];
        for ([_]Base{ .none, .{ .bytes = base_bytes[start..end], .slot = base_slot } }) |base| {
            var packed_buf: [bound(@sizeOf(@TypeOf(words)))]u8 = undefined;
            const packed_len = pack(&packed_buf, src, slot, base);
            try std.testing.expect(packed_len <= bound(src.len));
            var restored: [@sizeOf(@TypeOf(words))]u8 align(8) = undefined;
            const dst = restored[start..end];
            try std.testing.expectEqual(packed_len, unpack(packed_buf[0..packed_len], dst, slot, base));
            try std.testing.expectEqualSlices(u8, src, dst);
        }
    };
    var packed_buf: [bound(@sizeOf(@TypeOf(words)))]u8 = undefined;
    // Without a base: 8 zero words, 8 slot offsets, 8 u32s, 16 u64s.
    try std.testing.expectEqual(10 + 8 * 2 + 8 * 4 + 16 * 8, pack(&packed_buf, bytes, slot, .none));
}

test "base" {
    const slot: usize = 0x7000_0000_0000;
    const base_slot: usize = 0x7000_0010_0000;
    const words = [_]u64{
        0x1234_5678_9abc, // equals the base
        slot + 0x40, // equals the base once relocated
        base_slot + 0x40, // into the base's slot: the base relocates, so it differs (a u64)
        7, // differs: a u32
        slot + 0x80, // differs: a slot offset
        0xdead_beef_0000, // differs: a u64
    };
    const base_words = [_]u64{ 0x1234_5678_9abc, base_slot + 0x40, base_slot + 0x40, 8, 0, 0 };
    const base: Base = .{ .bytes = std.mem.sliceAsBytes(&base_words), .slot = base_slot };
    var packed_buf: [bound(@sizeOf(@TypeOf(words)))]u8 = undefined;
    const packed_len = pack(&packed_buf, std.mem.sliceAsBytes(&words), slot, base);
    try std.testing.expectEqual(2 + 8 + 4 + 2 + 8, packed_len);
    var restored: [words.len]u64 = undefined;
    _ = unpack(packed_buf[0..packed_len], std.mem.sliceAsBytes(&restored), slot, base);
    try std.testing.expectEqualSlices(u64, &words, &restored);
}

const std = @import("std");
