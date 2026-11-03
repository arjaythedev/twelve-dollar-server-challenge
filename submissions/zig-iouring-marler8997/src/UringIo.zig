//! A single-threaded `std.Io` on io_uring where every task is a fiber.
//!
//! A task blocks by queueing an SQE whose user_data is its fiber and switching
//! to the scheduler. The scheduler runs ready fibers until none are left, then
//! one io_uring_enter submits everything queued and waits for a completion.
//!
//! Fiber stacks are slots in one MAP_NORESERVE region. A fiber that blocks in
//! a read (the next request on a keep-alive connection, which can take seconds)
//! is "parked": its live stack bytes, minus the read's destination, are copied
//! to a small heap block and the slot's pages go back to the kernel. When the
//! read completes the bytes are copied back to the same addresses. An idle
//! connection costs about a kilobyte instead of a few stack pages. Parking
//! waits until a few hundred newer fibers wait in reads, so a busy
//! connection's next request usually arrives first and skips it.
//!
//! `callOnSchedulerStack` runs deep work that never suspends (SQLite, JSON) on
//! the scheduler's stack, so fiber stacks stay a page or two deep.
//!
//! With zig-staticio, `std_options.io_vtable = .{ .custom = UringIo }` makes
//! every `std.Io` call a direct call to the function of the same name here.
//! Only what the server reaches is implemented; a missing one is a compile
//! error.

const UringIo = @This();

ring: IoUring,
/// The scheduler's context while a fiber runs.
scheduler: Io.fiber.Context = undefined,
current: ?*Fiber = null,
/// Set by a fiber right before it switches to the scheduler.
yield_action: YieldAction = .none,
ready: FiberQueue = .{},
/// Fibers waiting in a read whose stacks are not packed yet, oldest first.
/// See `deferPark`.
unpacked: FiberList = .{},
futex_waiters: ?*Fiber = null,
on_scheduler_stack: bool = false,

stacks: [*]align(page_size) u8,
fibers: [*]Fiber,
slot_count: u32,
slots_used: u32 = 0,
free_slots: ?*Fiber = null,
live_fibers: u32 = 0,

accept: Accept = .{},
cancel_protection: Io.CancelProtection = .unblocked,
batch: InitOptions.Batch,
/// The last wait returned completions: wait for a batch next time.
busy: bool = false,

const page_size = std.heap.page_size_min;
/// Virtual size of one fiber stack. Only touched pages cost memory, deep work
/// runs on the scheduler stack, so this is a generous upper bound.
pub const stack_size = 64 * 1024;

const YieldAction = union(enum) {
    none,
    /// Save the fiber's stack except `[]u8` (a pending read's destination).
    park: []u8,
    finish,
};

const Fiber = struct {
    context: Io.fiber.Context,
    /// Ready queue, futex waiters, or the free list.
    next: ?*Fiber = null,
    /// The result of the last completion.
    result: i32 = 0,
    group: ?*Io.Group = null,
    futex: ?*const u32 = null,
    /// The packed stack (see `stack_pack`) of a parked fiber.
    parked: ?[*]u8 = null,
    /// The two saved ranges (below and above the excluded read destination),
    /// as offsets into the slot.
    parked_lo: Range = .{},
    parked_hi: Range = .{},
    /// Whether the stack was packed against `park_base`.
    parked_on_base: bool = false,
    /// Whether this fiber has done a read, see `park`.
    did_read: bool = false,
    /// Whether it is in `UringIo.unpacked` (`next` and `prev` link it).
    unpacked: bool = false,
    prev: ?*Fiber = null,

    const Range = struct { start: u32 = 0, len: u32 = 0 };
};

const FiberList = struct {
    head: ?*Fiber = null,
    tail: ?*Fiber = null,
    len: u32 = 0,

    fn append(l: *FiberList, f: *Fiber) void {
        f.prev = l.tail;
        f.next = null;
        if (l.tail) |t| t.next = f else l.head = f;
        l.tail = f;
        l.len += 1;
    }
    fn remove(l: *FiberList, f: *Fiber) void {
        if (f.prev) |p| p.next = f.next else l.head = f.next;
        if (f.next) |n| n.prev = f.prev else l.tail = f.prev;
        f.prev = null;
        f.next = null;
        l.len -= 1;
    }
};

const FiberQueue = struct {
    head: ?*Fiber = null,
    tail: ?*Fiber = null,

    fn push(q: *FiberQueue, f: *Fiber) void {
        f.next = null;
        if (q.tail) |t| t.next = f else q.head = f;
        q.tail = f;
    }
    fn pop(q: *FiberQueue) ?*Fiber {
        const f = q.head orelse return null;
        q.head = f.next;
        if (q.head == null) q.tail = null;
        f.next = null;
        return f;
    }
};

const Accept = struct {
    fds: [1024]linux.fd_t = undefined,
    head: u32 = 0,
    len: u32 = 0,
    listener: linux.fd_t = -1,
    armed: bool = false,
    backoff: bool = false,
    backoff_ts: linux.kernel_timespec = .{ .sec = 0, .nsec = 100 * std.time.ns_per_ms },
    waiter: ?*Fiber = null,
    last_error: linux.E = .SUCCESS,
};

// user_data values that are not fiber pointers
const ud_ignore = 0;
const ud_accept = 1;
const ud_accept_rearm = 2;

pub const InitOptions = struct {
    max_fibers: u32,
    batch: Batch = .{},
    sq_entries: u16 = 4096,
    cq_entries: u32 = 32768,

    /// While busy (the last wait returned completions), wait for `size`
    /// completions or `wait_ns`, whichever comes first, instead of waking
    /// for every completion. Idle, block for one. `size` 1 turns it off.
    pub const Batch = struct {
        size: u32 = 1,
        wait_ns: u32 = 0,
    };
};

pub fn init(io: *UringIo, options: InitOptions) !void {
    var params = std.mem.zeroInit(linux.io_uring_params, .{
        .flags = linux.IORING_SETUP_SINGLE_ISSUER | linux.IORING_SETUP_DEFER_TASKRUN |
            linux.IORING_SETUP_COOP_TASKRUN | linux.IORING_SETUP_SUBMIT_ALL |
            linux.IORING_SETUP_CQSIZE,
        .cq_entries = options.cq_entries,
    });
    const ring = try IoUring.init_params(options.sq_entries, &params);
    const stacks_len = @as(usize, options.max_fibers) * stack_size;
    const stacks = try mapNoReserve(stacks_len);
    const fibers = try mapNoReserve(@as(usize, options.max_fibers) * @sizeOf(Fiber));
    io.* = .{
        .ring = ring,
        .stacks = stacks,
        .fibers = @ptrCast(@alignCast(fibers)),
        .slot_count = options.max_fibers,
        .batch = options.batch,
    };
}

fn mapNoReserve(len: usize) ![*]align(page_size) u8 {
    const rc = linux.mmap(null, len, .{ .READ = true, .WRITE = true }, .{
        .TYPE = .PRIVATE,
        .ANONYMOUS = true,
        .NORESERVE = true,
    }, -1, 0);
    switch (linux.errno(rc)) {
        .SUCCESS => return @ptrFromInt(rc),
        else => |e| {
            std.log.err("mmap {d} bytes failed: {t}", .{ len, e });
            return error.OutOfMemory;
        },
    }
}

/// The one instance: `std_options_debug_io` points std's debug output here.
pub var instance: UringIo = undefined;

pub fn ioFromInstance() Io {
    return .{ .userdata = &instance };
}

fn fromUserdata(userdata: ?*anyopaque) *UringIo {
    return @ptrCast(@alignCast(userdata));
}

// ---------------------------------------------------------------------------
// Scheduler

/// Runs `func(args)` as the root task and returns once it returns, other tasks
/// may still be alive.
pub fn run(io: *UringIo, func: anytype, args: std.meta.ArgsTuple(@TypeOf(func))) void {
    const Args = @TypeOf(args);
    var done = false;
    const Root = struct {
        args: Args,
        done: *bool,
        fn start(ctx: *const anyopaque) void {
            const r: *const @This() = @ptrCast(@alignCast(ctx));
            @call(.auto, func, r.args);
            r.done.* = true;
        }
    };
    const root: Root = .{ .args = args, .done = &done };
    io.spawn(null, @ptrCast(&root), .of(Root), @sizeOf(Root), Root.start) catch
        @panic("no fiber for the root task");
    while (!done) io.tick();
}

/// Runs every ready fiber, then submits and waits for at least one completion.
fn tick(io: *UringIo) void {
    while (io.ready.pop()) |f| io.resume_(f);
    if (io.live_fibers == 0) return;
    io.submitAndWait();
    io.reap();
}

fn resume_(io: *UringIo, f: *Fiber) void {
    // Restored only now, not when the completion arrives: when every
    // connection closes at once, stacks come back one at a time.
    if (f.parked != null) io.unpark(f);
    io.current = f;
    io.yield_action = .none;
    switchContext(&io.scheduler, &f.context);
    io.current = null;
    switch (io.yield_action) {
        .none => {},
        .park => |dest| io.deferPark(f, dest),
        .finish => io.destroy(f),
    }
}

fn yield(io: *UringIo, action: YieldAction) void {
    const f = io.current.?;
    if (io.on_scheduler_stack) @panic("a task suspended inside callOnSchedulerStack");
    if (@frameAddress() < io.slotBottom(f) + 4096) @panic("fiber stack overflow");
    io.yield_action = action;
    switchContext(&f.context, &io.scheduler);
}

/// See `InitOptions.batch`.
fn submitAndWait(io: *UringIo) void {
    const to_submit = io.ring.flush_sq();
    var ts: linux.kernel_timespec = .{ .sec = 0, .nsec = io.batch.wait_ns };
    var arg: linux.io_uring_getevents_arg = .{ .sigmask = 0, .sigmask_sz = 0, .pad = 0, .ts = @intFromPtr(&ts) };
    const rc = if (io.busy and io.batch.size > 1) linux.syscall6(
        .io_uring_enter,
        @as(u32, @bitCast(io.ring.fd)),
        to_submit,
        io.batch.size,
        linux.IORING_ENTER_GETEVENTS | linux.IORING_ENTER_EXT_ARG,
        @intFromPtr(&arg),
        @sizeOf(linux.io_uring_getevents_arg),
    ) else linux.io_uring_enter(io.ring.fd, to_submit, 1, linux.IORING_ENTER_GETEVENTS, null);
    switch (linux.errno(rc)) {
        // A timeout or signal just ends the wait; reap what is there.
        .SUCCESS, .TIME, .INTR => {},
        // Completions are pending, reap them and enter again next tick.
        .AGAIN, .BUSY => {},
        else => |e| std.debug.panic("io_uring_enter: {t}", .{e}),
    }
}

fn reap(io: *UringIo) void {
    var cqes: [256]linux.io_uring_cqe = undefined;
    io.busy = false;
    while (true) {
        const n = io.ring.copy_cqes(&cqes, 0) catch |err| std.debug.panic("io_uring copy_cqes: {t}", .{err});
        if (n != 0) io.busy = true;
        for (cqes[0..n]) |cqe| io.complete(cqe);
        if (n < cqes.len) return;
    }
}

fn complete(io: *UringIo, cqe: linux.io_uring_cqe) void {
    switch (cqe.user_data) {
        ud_ignore => if (cqe.res < 0) std.log.warn("io_uring op failed: {t}", .{errnoOf(cqe.res)}),
        ud_accept => io.acceptCompleted(cqe),
        ud_accept_rearm => {
            io.accept.backoff = false;
            io.armAccept();
        },
        else => {
            const f: *Fiber = @ptrFromInt(cqe.user_data);
            f.result = cqe.res;
            io.makeReady(f);
        },
    }
}

fn makeReady(io: *UringIo, f: *Fiber) void {
    if (f.unpacked) {
        // Its read completed before the stack was packed: it still has it.
        io.unpacked.remove(f);
        f.unpacked = false;
    }
    io.ready.push(f);
}

/// Queues an SQE, flushing the submission queue to the kernel when it is full.
fn getSqe(io: *UringIo) *linux.io_uring_sqe {
    while (true) {
        return io.ring.get_sqe() catch {
            _ = io.ring.submit() catch |err| std.debug.panic("io_uring submit: {t}", .{err});
            continue;
        };
    }
}

/// Queues `sqe` for the current fiber, suspends it, and returns the result.
fn wait(io: *UringIo, sqe: *linux.io_uring_sqe, action: YieldAction) i32 {
    const f = io.current.?;
    sqe.user_data = @intFromPtr(f);
    io.yield(action);
    return f.result;
}

// ---------------------------------------------------------------------------
// Fibers

fn slotIndex(io: *UringIo, f: *Fiber) usize {
    return f - io.fibers;
}
fn slotBottom(io: *UringIo, f: *Fiber) usize {
    return @intFromPtr(io.stacks) + io.slotIndex(f) * stack_size;
}
fn slotTop(io: *UringIo, f: *Fiber) usize {
    return io.slotBottom(f) + stack_size;
}

const Closure = struct {
    io: *UringIo,
    start: *const fn (context: *const anyopaque) void,
    context: [*]const u8,

    /// The first code a fiber runs. Its stack pointer is just below the
    /// `Closure` (see `spawn`).
    fn entry() callconv(.naked) void {
        switch (builtin.cpu.arch) {
            .x86_64 => asm volatile (
                \\ leaq 8(%%rsp), %%rdi
                \\ jmp %[call:P]
                :
                : [call] "X" (&call),
            ),
            .aarch64 => asm volatile (
                \\ mov lr, #0
                \\ mov x0, sp
                \\ b %[call]
                :
                : [call] "X" (&call),
            ),
            else => comptime unreachable,
        }
    }

    fn call(closure: *Closure) callconv(.c) noreturn {
        closure.start(closure.context);
        closure.io.yield(.finish);
        unreachable;
    }
};

fn spawn(
    io: *UringIo,
    group: ?*Io.Group,
    context: [*]const u8,
    context_alignment: std.mem.Alignment,
    context_len: usize,
    start: *const fn (context: *const anyopaque) void,
) error{ConcurrencyUnavailable}!void {
    const f: *Fiber = if (io.free_slots) |f| blk: {
        io.free_slots = f.next;
        break :blk f;
    } else blk: {
        if (io.slots_used == io.slot_count) return error.ConcurrencyUnavailable;
        io.slots_used += 1;
        break :blk &io.fibers[io.slots_used - 1];
    };
    const top = io.slotTop(f);
    const context_addr = context_alignment.backward(top - context_len);
    @memcpy(@as([*]u8, @ptrFromInt(context_addr))[0..context_len], context[0..context_len]);
    const closure_addr = std.mem.alignBackward(usize, context_addr - @sizeOf(Closure), 16);
    const closure: *Closure = @ptrFromInt(closure_addr);
    closure.* = .{ .io = io, .start = start, .context = @ptrFromInt(context_addr) };
    f.* = .{
        .context = switch (builtin.cpu.arch) {
            // as if `entry` had been called: the return address slot is below the closure
            .x86_64 => .{ .rsp = closure_addr - 8, .rbp = 0, .rip = @intFromPtr(&Closure.entry) },
            .aarch64 => .{ .sp = closure_addr, .fp = 0, .pc = @intFromPtr(&Closure.entry) },
            else => comptime unreachable,
        },
        .group = group,
    };
    io.live_fibers += 1;
    io.ready.push(f);
}

fn destroy(io: *UringIo, f: *Fiber) void {
    io.live_fibers -= 1;
    if (f.group) |g| {
        g.state -= 1;
        if (g.state == 0) {
            const token = g.token.raw.?;
            g.token.raw = null;
            if (token != @as(*anyopaque, g)) io.ready.push(@ptrCast(@alignCast(token)));
        }
    }
    io.releaseStack(f);
    f.next = io.free_slots;
    io.free_slots = f;
}

/// Parks `f` (which just suspended on a read into `dest`) once
/// `max_unpacked` newer fibers are waiting in reads. A busy connection's next
/// request usually comes first: it then skips packing, releasing the pages and
/// faulting them back in, which cost more than the rest of a small request.
fn deferPark(io: *UringIo, f: *Fiber, dest: []u8) void {
    const bottom = io.slotBottom(f);
    // build.zig turns off the red zone: nothing live is below the stack pointer.
    const low = stackPointer(&f.context);
    const top = io.slotTop(f);
    const dest_start = std.math.clamp(@intFromPtr(dest.ptr), low, top);
    const dest_end = std.math.clamp(@intFromPtr(dest.ptr) + dest.len, dest_start, top);
    f.parked_lo = .{ .start = @intCast(low - bottom), .len = @intCast(dest_start - low) };
    f.parked_hi = .{ .start = @intCast(dest_end - bottom), .len = @intCast(top - dest_end) };
    io.unpacked.append(f);
    f.unpacked = true;
    if (io.unpacked.len > max_unpacked) {
        const oldest = io.unpacked.head.?;
        io.unpacked.remove(oldest);
        oldest.unpacked = false;
        io.park(oldest);
    }
}

/// Stacks that are kept resident this way cost a page or two each.
const max_unpacked = 256;

/// Saves the stack of `f`, which is waiting in a read, except the read's
/// destination (between `parked_lo` and `parked_hi`), and releases its pages.
/// The destination is excluded because the read overwrites it, and the part
/// it doesn't fill is unspecified, as with any `net_read`.
/// The read is still pending: with DEFER_TASKRUN the kernel fills the
/// destination only inside io_uring_enter, and a completed read takes the
/// fiber out of `unpacked` first, so no received bytes are released.
fn park(io: *UringIo, f: *Fiber) void {
    const bottom = io.slotBottom(f);
    const lo = io.slotBytes(f, f.parked_lo);
    const hi = io.slotBytes(f, f.parked_hi);
    if (park_base.len == 0 and lo.len + hi.len <= park_base.bytes.len) {
        @memcpy(park_base.bytes[0..lo.len], lo);
        @memcpy(park_base.bytes[lo.len..][0..hi.len], hi);
        park_base.len = lo.len + hi.len;
        park_base.lo = f.parked_lo;
        park_base.hi = f.parked_hi;
        park_base.slot = bottom;
    }
    f.parked_on_base = park_base.matches(f);
    const base_lo, const base_hi = park_base.get(f.parked_on_base);
    var n = stack_pack.pack(&pack_buffer, lo, bottom, base_lo);
    n += stack_pack.pack(pack_buffer[n..], hi, bottom, base_hi);
    const block = std.heap.c_allocator.alloc(u8, n) catch return; // stay resident
    @memcpy(block, pack_buffer[0..n]);
    f.parked = block.ptr;
    io.releaseStack(f);
}

/// Packs a parked stack before it is copied to a block of the right size.
var pack_buffer: [stack_pack.bound(stack_size)]u8 = undefined;

/// A copy of the first parked stack. Stacks later parked at the same offsets
/// are packed against it (see `stack_pack.Base`): connections park in the same
/// read with the same frames, and ~85% of their words match.
var park_base: struct {
    bytes: [4096]u8 = undefined,
    /// 0 until set.
    len: usize = 0,
    lo: Fiber.Range = .{},
    hi: Fiber.Range = .{},
    slot: usize = 0,

    fn matches(b: *const @This(), f: *const Fiber) bool {
        return b.len != 0 and std.meta.eql(b.lo, f.parked_lo) and std.meta.eql(b.hi, f.parked_hi);
    }

    /// The bases for the low and high range.
    fn get(b: *const @This(), on_base: bool) [2]stack_pack.Base {
        if (!on_base) return .{ .none, .none };
        return .{
            .{ .bytes = b.bytes[0..b.lo.len], .slot = b.slot },
            .{ .bytes = b.bytes[b.lo.len..][0..b.hi.len], .slot = b.slot },
        };
    }
} = .{};

comptime {
    std.debug.assert(stack_size <= stack_pack.max_slot_size);
}

fn slotBytes(io: *UringIo, f: *Fiber, range: Fiber.Range) []u8 {
    return @as([*]u8, @ptrFromInt(io.slotBottom(f) + range.start))[0..range.len];
}

/// Returns all of a slot's pages to the kernel: calls deeper than where the
/// fiber suspended (or finished) touched pages below its stack pointer.
fn releaseStack(io: *UringIo, f: *Fiber) void {
    _ = linux.madvise(@ptrFromInt(io.slotBottom(f)), stack_size, linux.MADV.DONTNEED);
}

fn unpark(io: *UringIo, f: *Fiber) void {
    const block = f.parked.?;
    const bottom = io.slotBottom(f);
    // The block's length isn't stored: unpacking finds where it ends.
    const unbounded = block[0..std.math.maxInt(usize)];
    const base_lo, const base_hi = park_base.get(f.parked_on_base);
    var n = stack_pack.unpack(unbounded, io.slotBytes(f, f.parked_lo), bottom, base_lo);
    n += stack_pack.unpack(unbounded[n..], io.slotBytes(f, f.parked_hi), bottom, base_hi);
    std.heap.c_allocator.free(block[0..n]);
    f.parked = null;
}

/// Calls `func(args)` on the scheduler's (main thread) stack, which is deep and
/// always resident. `func` must not suspend (no `Io` calls that block).
pub fn callOnSchedulerStack(io_: Io, comptime func: anytype, args: std.meta.ArgsTuple(@TypeOf(func))) @typeInfo(@TypeOf(func)).@"fn".return_type.? {
    const io = fromUserdata(io_.userdata);
    if (io.current == null or io.on_scheduler_stack) return @call(.auto, func, args);
    const Result = @typeInfo(@TypeOf(func)).@"fn".return_type.?;
    const Call = struct {
        args: @TypeOf(args),
        result: Result = undefined,
        fn run(c: *@This()) callconv(.c) void {
            c.result = @call(.auto, func, c.args);
        }
    };
    var c: Call = .{ .args = args };
    io.on_scheduler_stack = true;
    defer io.on_scheduler_stack = false;
    const top = std.mem.alignBackward(usize, stackPointer(&io.scheduler) - 64, 16);
    switch (builtin.cpu.arch) {
        .x86_64 => asm volatile (
            \\ movq %%rsp, %%rbx
            \\ movq %[top], %%rsp
            \\ callq *%[func]
            \\ movq %%rbx, %%rsp
            :
            : [top] "{r12}" (top),
              [func] "{r13}" (&Call.run),
              [arg] "{rdi}" (&c),
            : call_clobbers),
        .aarch64 => asm volatile (
            \\ mov x19, sp
            \\ mov sp, %[top]
            \\ blr %[func]
            \\ mov sp, x19
            :
            : [top] "{x20}" (top),
              [func] "{x21}" (&Call.run),
              [arg] "{x0}" (&c),
            : call_clobbers),
        else => comptime unreachable,
    }
    return c.result;
}

fn stackPointer(context: *const Io.fiber.Context) usize {
    return switch (builtin.cpu.arch) {
        .x86_64 => context.rsp,
        .aarch64 => context.sp,
        else => comptime unreachable,
    };
}

/// Saves the current context in `old` and resumes `new`.
///
/// Like `std.Io.fiber.contextSwitch`, but it declares the vector registers the
/// target actually has as clobbered: std's list names only zmm0-31 (z0-31 on
/// aarch64), which LLVM ignores without AVX-512 (SVE), so values kept in vector
/// registers across a switch got corrupted by whatever ran in between.
inline fn switchContext(old: *Io.fiber.Context, new: *Io.fiber.Context) void {
    switch (builtin.cpu.arch) {
        .x86_64 => asm volatile (
            \\ leaq 0f(%%rip), %%rdx
            \\ movq %%rsp, 0(%%rax)
            \\ movq %%rbp, 8(%%rax)
            \\ movq %%rdx, 16(%%rax)
            \\ movq 0(%%rcx), %%rsp
            \\ movq 8(%%rcx), %%rbp
            \\ jmpq *16(%%rcx)
            \\0:
            :
            : [old] "{rax}" (old),
              [new] "{rcx}" (new),
            : switch_clobbers),
        .aarch64 => asm volatile (
            \\ adr x2, 0f
            \\ mov x3, sp
            \\ stp x3, fp, [x0]
            \\ str x2, [x0, #16]
            \\ ldp x3, fp, [x1]
            \\ ldr x2, [x1, #16]
            \\ mov sp, x3
            \\ br x2
            \\0:
            :
            : [old] "{x0}" (old),
              [new] "{x1}" (new),
            : switch_clobbers),
        else => comptime unreachable,
    }
}

/// Everything but the stack and frame pointers (which the switch saves) is clobbered.
const switch_clobbers = switch (builtin.cpu.arch) {
    .x86_64 => x86Clobbers(&.{ "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" }),
    .aarch64 => aarch64Clobbers(28),
    else => @compileError("UringIo supports x86_64 and aarch64, not " ++ @tagName(builtin.cpu.arch)),
};
/// A call clobbers the caller-saved registers, plus the one holding the old
/// stack pointer (rbx, x19).
const call_clobbers = switch (builtin.cpu.arch) {
    .x86_64 => x86Clobbers(&.{ "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11" }),
    .aarch64 => aarch64Clobbers(19),
    else => unreachable,
};

fn x86Clobbers(comptime gprs: []const []const u8) std.lang.assembly.Clobbers {
    @setEvalBranchQuota(100_000); // batch 2: AVX-512 CPUs loop over 32 zmm registers at comptime
    var c: std.lang.assembly.Clobbers = .{ .memory = true, .fpsr = true, .fpcr = true, .mxcsr = true, .rflags = true, .dirflag = true };
    for (gprs) |r| @field(c, r) = true;
    const features = builtin.cpu.features;
    const has = std.Target.x86.featureSetHas;
    const vector_prefix, const vector_count = if (has(features, .avx512f))
        .{ "zmm", 32 }
    else if (has(features, .avx))
        .{ "ymm", 16 }
    else
        .{ "xmm", 16 };
    for (0..vector_count) |i| @field(c, std.fmt.comptimePrint("{s}{d}", .{ vector_prefix, i })) = true;
    for (0..8) |i| @field(c, std.fmt.comptimePrint("mm{d}", .{i})) = true;
    for (0..8) |i| @field(c, std.fmt.comptimePrint("st{d}", .{i})) = true;
    return c;
}

/// Clobbers x0 through `last_gpr` and the link register. x18 is skipped when
/// the target reserves it. All vector registers are clobbered in full, which
/// also covers the callee-saved low halves of v8-v15.
fn aarch64Clobbers(comptime last_gpr: u5) std.lang.assembly.Clobbers {
    @setEvalBranchQuota(100_000);
    var c: std.lang.assembly.Clobbers = .{ .memory = true, .nzcv = true, .fpsr = true, .fpcr = true, .lr = true };
    const features = builtin.cpu.features;
    const has = std.Target.aarch64.featureSetHas;
    for (0..@as(usize, last_gpr) + 1) |i| {
        if (i == 18 and has(features, .reserve_x18)) continue;
        @field(c, std.fmt.comptimePrint("x{d}", .{i})) = true;
    }
    for (0..32) |i| @field(c, std.fmt.comptimePrint("v{d}", .{i})) = true;
    if (has(features, .sve)) {
        for (0..32) |i| @field(c, std.fmt.comptimePrint("z{d}", .{i})) = true;
        for (0..16) |i| @field(c, std.fmt.comptimePrint("p{d}", .{i})) = true;
    }
    return c;
}

// ---------------------------------------------------------------------------
// std.Io.VTable

pub fn crashHandler(userdata: ?*anyopaque) void {
    _ = userdata;
}

pub fn groupConcurrent(
    userdata: ?*anyopaque,
    group: *Io.Group,
    context: []const u8,
    context_alignment: std.mem.Alignment,
    start: *const fn (context: *const anyopaque) void,
) Io.ConcurrentError!void {
    const io = fromUserdata(userdata);
    try io.spawn(group, context.ptr, context_alignment, context.len, start);
    if (group.token.raw == null) group.token.raw = group;
    group.state += 1;
}

pub fn groupAsync(
    userdata: ?*anyopaque,
    group: *Io.Group,
    context: []const u8,
    context_alignment: std.mem.Alignment,
    start: *const fn (context: *const anyopaque) void,
) void {
    groupConcurrent(userdata, group, context, context_alignment, start) catch start(context.ptr);
}

pub fn groupAwait(userdata: ?*anyopaque, group: *Io.Group, token: *anyopaque) Io.Cancelable!void {
    const io = fromUserdata(userdata);
    if (token != @as(*anyopaque, group)) @panic("UringIo: one awaiter per group");
    group.token.raw = io.current.?;
    io.yield(.none);
}

pub fn recancel(userdata: ?*anyopaque) void {
    _ = userdata;
}

pub fn swapCancelProtection(userdata: ?*anyopaque, new: Io.CancelProtection) Io.CancelProtection {
    const io = fromUserdata(userdata);
    defer io.cancel_protection = new;
    return io.cancel_protection;
}

/// Tasks are never canceled.
pub fn checkCancel(userdata: ?*anyopaque) Io.Cancelable!void {
    _ = userdata;
}

pub fn futexWait(userdata: ?*anyopaque, ptr: *const u32, expected: u32, timeout: Io.Timeout) Io.Cancelable!void {
    if (timeout != .none) @panic("UringIo: futexWait timeouts are not implemented");
    futexWaitUncancelable(userdata, ptr, expected);
}

pub fn futexWaitUncancelable(userdata: ?*anyopaque, ptr: *const u32, expected: u32) void {
    const io = fromUserdata(userdata);
    if (@atomicLoad(u32, ptr, .monotonic) != expected) return;
    const f = io.current orelse @panic("UringIo: futexWait outside of a task would deadlock");
    f.futex = ptr;
    f.next = io.futex_waiters;
    io.futex_waiters = f;
    io.yield(.none);
}

pub fn futexWake(userdata: ?*anyopaque, ptr: *const u32, max_waiters: u32) void {
    const io = fromUserdata(userdata);
    var woken: u32 = 0;
    var link = &io.futex_waiters;
    while (link.*) |f| {
        if (woken == max_waiters) break;
        if (f.futex == ptr) {
            link.* = f.next;
            f.futex = null;
            io.ready.push(f);
            woken += 1;
        } else link = &f.next;
    }
}

pub fn operate(userdata: ?*anyopaque, operation: Io.Operation) Io.Cancelable!Io.Operation.Result {
    const io = fromUserdata(userdata);
    return switch (operation) {
        .net_read => |o| .{ .net_read = io.netRead(o) },
        .net_write => |o| .{ .net_write = io.netWrite(o) },
        .file_write_streaming => |o| .{ .file_write_streaming = fileWriteStreaming(o) },
        .file_read_streaming => |o| .{ .file_read_streaming = fileReadStreaming(o) },
        else => std.debug.panic("UringIo: operation {t} is not implemented", .{operation}),
    };
}

fn netRead(io: *UringIo, o: Io.Operation.NetRead) Io.Operation.NetRead.Error!net.Stream.ReadResult {
    if (o.control.len != 0) @panic("UringIo: net_read control data is not implemented");
    const dest = for (o.data) |d| {
        if (d.len != 0) break d;
    } else return .{ .data_len = 0 };
    const f = io.current.?;
    const sqe = io.getSqe();
    sqe.prep_recv(o.socket_handle, dest, 0);
    // The first read of a connection usually finds its request already there.
    // Later ones wait for the client's next request, seconds away: park, and
    // arm the poll without first trying a recv that would find nothing.
    if (f.did_read) sqe.ioprio |= linux.IORING_RECVSEND_POLL_FIRST;
    const action: YieldAction = if (f.did_read) .{ .park = dest } else .none;
    f.did_read = true;
    const rc = io.wait(sqe, action);
    if (rc >= 0) return .{ .data_len = @intCast(rc) };
    return switch (errnoOf(rc)) {
        .CONNRESET => error.ConnectionResetByPeer,
        .NOTCONN => error.SocketUnconnected,
        .TIMEDOUT => error.ConnectionTimedOut,
        .NOBUFS, .NOMEM => error.SystemResources,
        .NETDOWN => error.NetworkDown,
        else => |e| unexpected(e),
    };
}

/// noinline, like the file operations below: their iovec arrays would
/// otherwise sit in the caller's frame, and parking saves every frame.
noinline fn netWrite(io: *UringIo, o: Io.Operation.NetWrite) Io.Operation.NetWrite.Error!usize {
    if (o.control.len != 0) @panic("UringIo: net_write control data is not implemented");
    var iovecs: [16]std.posix.iovec_const = undefined;
    var n: usize = 0;
    if (o.header.len != 0) {
        iovecs[n] = .{ .base = o.header.ptr, .len = o.header.len };
        n += 1;
    }
    if (o.data.len != 0) {
        for (o.data[0 .. o.data.len - 1]) |d| {
            if (n == iovecs.len) break;
            if (d.len == 0) continue;
            iovecs[n] = .{ .base = d.ptr, .len = d.len };
            n += 1;
        }
        const last = o.data[o.data.len - 1];
        if (last.len != 0) for (0..o.splat) |_| {
            if (n == iovecs.len) break;
            iovecs[n] = .{ .base = last.ptr, .len = last.len };
            n += 1;
        };
    }
    if (n == 0) return 0;
    const sqe = io.getSqe();
    // One buffer (every response): SEND skips the msghdr and iovec import.
    if (n == 1) {
        sqe.prep_send(o.socket_handle, iovecs[0].base[0..iovecs[0].len], linux.MSG.NOSIGNAL);
        return netWriteResult(io.wait(sqe, .none));
    }
    const msg: linux.msghdr_const = .{
        .name = null,
        .namelen = 0,
        .iov = &iovecs,
        .iovlen = n,
        .control = null,
        .controllen = 0,
        .flags = 0,
    };
    sqe.prep_sendmsg(o.socket_handle, &msg, linux.MSG.NOSIGNAL);
    return netWriteResult(io.wait(sqe, .none));
}

fn netWriteResult(rc: i32) Io.Operation.NetWrite.Error!usize {
    if (rc >= 0) return @intCast(rc);
    return switch (errnoOf(rc)) {
        .CONNRESET, .PIPE => error.ConnectionResetByPeer,
        .NOTCONN => error.SocketUnconnected,
        .TIMEDOUT => error.ConnectionTimedOut,
        .NOBUFS, .NOMEM => error.SystemResources,
        .NETDOWN => error.NetworkDown,
        .NETUNREACH => error.NetworkUnreachable,
        .HOSTUNREACH => error.HostUnreachable,
        else => |e| unexpected(e),
    };
}

/// Blocking: only stderr logging reaches this.
noinline fn fileWriteStreaming(o: Io.Operation.FileWriteStreaming) Io.Operation.FileWriteStreaming.Error!usize {
    var iovecs: [16]std.posix.iovec_const = undefined;
    var n: usize = 0;
    if (o.header.len != 0) {
        iovecs[n] = .{ .base = o.header.ptr, .len = o.header.len };
        n += 1;
    }
    if (o.data.len != 0) {
        for (o.data[0 .. o.data.len - 1]) |d| {
            if (n == iovecs.len) break;
            iovecs[n] = .{ .base = d.ptr, .len = d.len };
            n += 1;
        }
        const last = o.data[o.data.len - 1];
        for (0..o.splat) |_| {
            if (n == iovecs.len) break;
            iovecs[n] = .{ .base = last.ptr, .len = last.len };
            n += 1;
        }
    }
    while (true) {
        const rc = linux.writev(o.file.handle, &iovecs, n);
        switch (linux.errno(rc)) {
            .SUCCESS => return rc,
            .INTR => continue,
            .AGAIN => return error.WouldBlock,
            .PIPE => return error.BrokenPipe,
            .NOSPC => return error.NoSpaceLeft,
            .IO => return error.InputOutput,
            else => |e| return unexpected(e),
        }
    }
}

/// Blocking: only startup file reads (the load generator's tokens) reach this.
noinline fn fileReadStreaming(o: Io.Operation.FileReadStreaming) Io.Operation.FileReadStreaming.Error!usize {
    var iovecs: [16]std.posix.iovec = undefined;
    var n: usize = 0;
    for (o.data) |d| {
        if (n == iovecs.len) break;
        if (d.len == 0) continue;
        iovecs[n] = .{ .base = d.ptr, .len = d.len };
        n += 1;
    }
    if (n == 0) return 0;
    while (true) {
        const rc = linux.readv(o.file.handle, &iovecs, n);
        switch (linux.errno(rc)) {
            .SUCCESS => return if (rc == 0) error.EndOfStream else rc,
            .INTR => continue,
            .ISDIR => return error.IsDir,
            .IO => return error.InputOutput,
            .NOMEM => return error.SystemResources,
            else => |e| return unexpected(e),
        }
    }
}

pub fn netConnectIp(
    userdata: ?*anyopaque,
    address: *const net.IpAddress,
    options: net.IpAddress.ConnectOptions,
) net.IpAddress.ConnectError!net.Socket {
    const io = fromUserdata(userdata);
    if (options.mode != .stream or (options.protocol != null and options.protocol != .tcp)) return error.OptionUnsupported;
    if (options.timeout != .none) return error.OptionUnsupported;
    var storage: Io.Threaded.PosixAddress = undefined;
    const len = Io.Threaded.addressToPosix(address, &storage);
    const rc = linux.socket(Io.Threaded.posixAddressFamily(address), linux.SOCK.STREAM | linux.SOCK.CLOEXEC, linux.IPPROTO.TCP);
    const fd: linux.fd_t = switch (linux.errno(rc)) {
        .SUCCESS => @intCast(rc),
        .MFILE => return error.ProcessFdQuotaExceeded,
        .NFILE => return error.SystemFdQuotaExceeded,
        .NOBUFS, .NOMEM => return error.SystemResources,
        .AFNOSUPPORT => return error.AddressFamilyUnsupported,
        else => |e| return unexpected(e),
    };
    errdefer _ = linux.close(fd);
    try setsockopt(fd, linux.IPPROTO.TCP, linux.TCP.NODELAY, 1);
    const sqe = io.getSqe();
    sqe.prep_connect(fd, &storage.any, len);
    const res = io.wait(sqe, .none);
    if (res < 0) return switch (errnoOf(res)) {
        .ADDRNOTAVAIL => error.AddressUnavailable,
        .CONNREFUSED => error.ConnectionRefused,
        .CONNRESET => error.ConnectionResetByPeer,
        .HOSTUNREACH => error.HostUnreachable,
        .NETUNREACH => error.NetworkUnreachable,
        .TIMEDOUT => error.Timeout,
        .NETDOWN => error.NetworkDown,
        .AGAIN, .NOBUFS, .NOMEM => error.SystemResources,
        .ACCES, .PERM => error.AccessDenied,
        else => |e| unexpected(e),
    };
    return .{ .handle = fd, .address = address.* };
}

pub fn netListenIp(
    userdata: ?*anyopaque,
    address: *const net.IpAddress,
    options: net.IpAddress.ListenOptions,
) net.IpAddress.ListenError!net.Socket {
    _ = userdata;
    if (options.mode != .stream or options.protocol != .tcp) return error.OptionUnsupported;
    var storage: Io.Threaded.PosixAddress = undefined;
    var len = Io.Threaded.addressToPosix(address, &storage);
    const family: u32 = Io.Threaded.posixAddressFamily(address);
    const rc = linux.socket(family, linux.SOCK.STREAM | linux.SOCK.CLOEXEC, linux.IPPROTO.TCP);
    const fd: linux.fd_t = switch (linux.errno(rc)) {
        .SUCCESS => @intCast(rc),
        .MFILE => return error.ProcessFdQuotaExceeded,
        .NFILE => return error.SystemFdQuotaExceeded,
        .NOBUFS, .NOMEM => return error.SystemResources,
        .AFNOSUPPORT => return error.AddressFamilyUnsupported,
        else => |e| return unexpected(e),
    };
    errdefer _ = linux.close(fd);
    const one: c_int = 1;
    if (options.reuse_address) try setsockopt(fd, linux.SOL.SOCKET, linux.SO.REUSEADDR, one);
    // Inherited by accepted sockets: every response goes out in one write.
    try setsockopt(fd, linux.IPPROTO.TCP, linux.TCP.NODELAY, one);
    switch (linux.errno(linux.bind(fd, &storage.any, len))) {
        .SUCCESS => {},
        .ACCES, .PERM => return error.AccessDenied,
        .ADDRINUSE => return error.AddressInUse,
        .ADDRNOTAVAIL => return error.AddressUnavailable,
        .NOMEM => return error.SystemResources,
        else => |e| return unexpected(e),
    }
    switch (linux.errno(linux.listen(fd, options.kernel_backlog))) {
        .SUCCESS => {},
        .ADDRINUSE => return error.AddressInUse,
        else => |e| return unexpected(e),
    }
    switch (linux.errno(linux.getsockname(fd, &storage.any, &len))) {
        .SUCCESS => {},
        else => |e| return unexpected(e),
    }
    return .{ .handle = fd, .address = Io.Threaded.addressFromPosix(&storage) };
}

fn setsockopt(fd: linux.fd_t, level: i32, opt: u32, value: c_int) error{Unexpected}!void {
    switch (linux.errno(linux.setsockopt(fd, level, opt, std.mem.asBytes(&value), @sizeOf(c_int)))) {
        .SUCCESS => {},
        else => |e| return unexpected(e),
    }
}

/// One multishot accept feeds a queue; it is re-armed after a pause if the
/// kernel stops it (out of file descriptors), so a full table never spins.
pub fn netAccept(userdata: ?*anyopaque, listener: net.Socket.Handle, options: net.Server.AcceptOptions) net.Server.AcceptError!net.Socket {
    _ = options;
    const io = fromUserdata(userdata);
    const a = &io.accept;
    if (a.listener != listener) {
        if (a.listener != -1) @panic("UringIo: accept on one listener only");
        a.listener = listener;
    }
    while (a.len == 0) {
        if (!a.armed and !a.backoff) io.armAccept();
        if (a.waiter != null) @panic("UringIo: one accepting task only");
        a.waiter = io.current.?;
        io.yield(.none);
    }
    const fd = a.fds[a.head];
    a.head = (a.head + 1) % @as(u32, a.fds.len);
    a.len -= 1;
    return .{ .handle = fd, .address = .{ .ip4 = .unspecified(0) } };
}

fn armAccept(io: *UringIo) void {
    const sqe = io.getSqe();
    sqe.prep_multishot_accept(io.accept.listener, null, null, linux.SOCK.CLOEXEC);
    sqe.user_data = ud_accept;
    io.accept.armed = true;
}

fn acceptCompleted(io: *UringIo, cqe: linux.io_uring_cqe) void {
    const a = &io.accept;
    if (cqe.flags & linux.IORING_CQE_F_MORE == 0) a.armed = false;
    if (cqe.res >= 0) {
        if (a.len == a.fds.len) {
            // The accepting task is starved; refuse rather than grow without bound.
            _ = linux.close(cqe.res);
        } else {
            a.fds[(a.head + a.len) % @as(u32, a.fds.len)] = cqe.res;
            a.len += 1;
        }
    } else {
        const e = errnoOf(cqe.res);
        if (e != a.last_error) std.log.err("accept: {t}", .{e});
        a.last_error = e;
    }
    if (!a.armed and !a.backoff) {
        if (cqe.res < 0) {
            a.backoff = true;
            const sqe = io.getSqe();
            sqe.prep_timeout(&a.backoff_ts, 0, 0);
            sqe.user_data = ud_accept_rearm;
        } else if (a.waiter != null) io.armAccept();
    }
    if (a.len != 0) if (a.waiter) |w| {
        a.waiter = null;
        io.ready.push(w);
    };
}

pub fn netClose(userdata: ?*anyopaque, sockets: []const net.Socket) void {
    _ = userdata;
    for (sockets) |s| _ = linux.close(s.handle);
}

pub fn now(userdata: ?*anyopaque, clock: Io.Clock) Io.Timestamp {
    _ = userdata;
    var ts: linux.timespec = undefined;
    const id: linux.clockid_t = switch (clock) {
        .real => .REALTIME,
        .awake => .MONOTONIC,
        .boot => .BOOTTIME,
        else => std.debug.panic("UringIo: clock {t} is not implemented", .{clock}),
    };
    // libc's goes through the vDSO; `linux.clock_gettime` is a syscall.
    if (std.c.clock_gettime(id, &ts) != 0) std.debug.panic("clock_gettime({t}) failed", .{id});
    return .{ .nanoseconds = @as(i96, ts.sec) * std.time.ns_per_s + ts.nsec };
}

pub fn sleep(userdata: ?*anyopaque, timeout: Io.Timeout) Io.Cancelable!void {
    const io = fromUserdata(userdata);
    const ns: i96 = switch (timeout) {
        .none => @panic("UringIo: sleep forever"),
        .duration => |d| d.raw.nanoseconds,
        .deadline => |d| d.raw.nanoseconds - now(userdata, d.clock).nanoseconds,
    };
    if (ns <= 0) return;
    const ts: linux.kernel_timespec = .{
        .sec = @intCast(@divTrunc(ns, std.time.ns_per_s)),
        .nsec = @intCast(@rem(ns, std.time.ns_per_s)),
    };
    const sqe = io.getSqe();
    sqe.prep_timeout(&ts, 0, 0);
    _ = io.wait(sqe, .none);
}

var stderr_writer: Io.File.Writer = .{
    .io = undefined,
    .interface = Io.File.Writer.initInterface(&.{}),
    .file = .stderr(),
    .mode = .streaming,
};
/// The checkpoint thread logs too. Held only for one blocking write.
var stderr_locked: std.atomic.Value(bool) = .init(false);

pub fn lockStderr(userdata: ?*anyopaque, terminal_mode: ?Io.Terminal.Mode) Io.Cancelable!Io.LockedStderr {
    while (stderr_locked.cmpxchgWeak(false, true, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
    return lockedStderr(userdata, terminal_mode);
}

pub fn tryLockStderr(userdata: ?*anyopaque, terminal_mode: ?Io.Terminal.Mode) Io.Cancelable!?Io.LockedStderr {
    if (stderr_locked.cmpxchgStrong(false, true, .acquire, .monotonic) != null) return null;
    return lockedStderr(userdata, terminal_mode);
}

fn lockedStderr(userdata: ?*anyopaque, terminal_mode: ?Io.Terminal.Mode) Io.LockedStderr {
    _ = terminal_mode;
    stderr_writer.io = .{ .userdata = userdata };
    return .{ .file_writer = &stderr_writer, .terminal_mode = .no_color };
}

pub fn unlockStderr(userdata: ?*anyopaque) void {
    _ = userdata;
    stderr_writer.interface.flush() catch {};
    stderr_writer.interface.end = 0;
    stderr_writer.interface.buffer = &.{};
    stderr_locked.store(false, .release);
}

// ---------------------------------------------------------------------------
// Blocking file operations. Only std.debug reaches these, to print a stack
// trace (reading the executable's debug info and the source lines).

pub fn dirOpenFile(userdata: ?*anyopaque, dir: Io.Dir, sub_path: []const u8, options: Io.Dir.OpenFileOptions) Io.File.OpenError!Io.File {
    _ = userdata;
    var path: [std.fs.max_path_bytes:0]u8 = undefined;
    if (sub_path.len >= path.len) return error.NameTooLong;
    @memcpy(path[0..sub_path.len], sub_path);
    path[sub_path.len] = 0;
    const flags: linux.O = .{
        .ACCMODE = switch (options.mode) {
            .read_only => .RDONLY,
            .write_only => .WRONLY,
            .read_write => .RDWR,
        },
        .CLOEXEC = true,
    };
    while (true) {
        const rc = linux.openat(dir.handle, &path, flags, 0);
        switch (linux.errno(rc)) {
            .SUCCESS => return .{ .handle = @intCast(rc), .flags = .{ .nonblocking = false } },
            .INTR => continue,
            .NOENT => return error.FileNotFound,
            .ACCES => return error.AccessDenied,
            .PERM => return error.PermissionDenied,
            .ISDIR => return error.IsDir,
            .NOTDIR => return error.NotDir,
            .MFILE => return error.ProcessFdQuotaExceeded,
            .NFILE => return error.SystemFdQuotaExceeded,
            .NOMEM => return error.SystemResources,
            .LOOP => return error.SymLinkLoop,
            else => |e| return unexpected(e),
        }
    }
}

pub fn fileClose(userdata: ?*anyopaque, files: []const Io.File) void {
    _ = userdata;
    for (files) |f| _ = linux.close(f.handle);
}

pub fn fileStat(userdata: ?*anyopaque, file: Io.File) Io.File.StatError!Io.File.Stat {
    _ = userdata;
    var stx = std.mem.zeroes(linux.Statx);
    switch (linux.errno(linux.statx(file.handle, "", linux.AT.EMPTY_PATH, Io.Threaded.linux_statx_request, &stx))) {
        .SUCCESS => return Io.Threaded.statFromLinux(&stx),
        .ACCES => return error.AccessDenied,
        .NOMEM => return error.SystemResources,
        else => |e| return unexpected(e),
    }
}

pub fn fileLength(userdata: ?*anyopaque, file: Io.File) Io.File.LengthError!u64 {
    return (try fileStat(userdata, file)).size;
}

pub fn fileSeekBy(userdata: ?*anyopaque, file: Io.File, offset: i64) Io.File.SeekError!void {
    _ = userdata;
    switch (linux.errno(linux.lseek(file.handle, offset, linux.SEEK.CUR))) {
        .SUCCESS => {},
        .SPIPE => return error.Unseekable,
        else => |e| return unexpected(e),
    }
}

pub fn fileReadPositional(userdata: ?*anyopaque, file: Io.File, data: []const []u8, offset: u64) Io.File.ReadPositionalError!usize {
    _ = userdata;
    var iovecs: [16]std.posix.iovec = undefined;
    var n: usize = 0;
    for (data) |d| {
        if (n == iovecs.len) break;
        if (d.len == 0) continue;
        iovecs[n] = .{ .base = d.ptr, .len = d.len };
        n += 1;
    }
    if (n == 0) return 0;
    while (true) {
        const rc = linux.preadv(file.handle, &iovecs, n, @bitCast(offset));
        switch (linux.errno(rc)) {
            .SUCCESS => return rc,
            .INTR => continue,
            .SPIPE => return error.Unseekable,
            .ISDIR => return error.IsDir,
            .IO => return error.InputOutput,
            .NOMEM => return error.SystemResources,
            else => |e| return unexpected(e),
        }
    }
}

pub fn fileWritePositional(userdata: ?*anyopaque, file: Io.File, header: []const u8, data: []const []const u8, splat: usize, offset: u64) Io.File.WritePositionalError!usize {
    _ = userdata;
    _ = file;
    _ = header;
    _ = data;
    _ = splat;
    _ = offset;
    return error.Unseekable; // stderr is written streaming
}

pub fn fileWriteFileStreaming(userdata: ?*anyopaque, file: Io.File, header: []const u8, reader: *Io.File.Reader, limit: Io.Limit) Io.File.Writer.WriteFileError!usize {
    _ = userdata;
    _ = file;
    _ = header;
    _ = reader;
    _ = limit;
    return error.Unimplemented;
}

pub fn fileWriteFilePositional(userdata: ?*anyopaque, file: Io.File, header: []const u8, reader: *Io.File.Reader, limit: Io.Limit, offset: u64) Io.File.WriteFilePositionalError!usize {
    _ = userdata;
    _ = file;
    _ = header;
    _ = reader;
    _ = limit;
    _ = offset;
    return error.Unimplemented;
}

pub fn fileSeekTo(userdata: ?*anyopaque, file: Io.File, offset: u64) Io.File.SeekError!void {
    _ = userdata;
    switch (linux.errno(linux.lseek(file.handle, @bitCast(offset), linux.SEEK.SET))) {
        .SUCCESS => {},
        .SPIPE => return error.Unseekable,
        else => |e| return unexpected(e),
    }
}

/// `net.Stream.Writer` falls back to plain writes.
pub fn netWriteFile(userdata: ?*anyopaque, handle: net.Socket.Handle, header: []const u8, reader: *Io.File.Reader, limit: Io.Limit) net.Stream.Writer.WriteFileError!usize {
    _ = userdata;
    _ = handle;
    _ = header;
    _ = reader;
    _ = limit;
    return error.Unimplemented;
}

pub fn processExecutablePath(userdata: ?*anyopaque, buffer: []u8) std.process.ExecutablePathError!usize {
    _ = userdata;
    const rc = linux.readlink("/proc/self/exe", buffer.ptr, buffer.len);
    switch (linux.errno(rc)) {
        .SUCCESS => {
            if (rc == buffer.len) return error.NameTooLong;
            return rc;
        },
        else => |e| return unexpected(e),
    }
}

fn errnoOf(rc: i32) linux.E {
    return @fromBackingInt(@intCast(-rc));
}

fn unexpected(e: linux.E) error{Unexpected} {
    std.log.err("unexpected errno: {t}", .{e});
    return error.Unexpected;
}

const builtin = @import("builtin");
const std = @import("std");
const Io = std.Io;
const stack_pack = @import("stack_pack.zig");
const net = Io.net;
const linux = std.os.linux;
const IoUring = linux.IoUring;
