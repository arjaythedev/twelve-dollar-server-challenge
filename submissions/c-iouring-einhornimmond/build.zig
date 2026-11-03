//! Builds bin/server: SQLite, libsodium, arnm and picohttpparser, all from pinned sources. The
//! Debian/Ubuntu libc handling is the one gradido-unicorn's fast-servers build uses.
const std = @import("std");
const builtin = @import("builtin");

/// True when @p target names the machine this build runs on.
///
/// Either by being native, or by spelling the host's own triple out --
/// `-Dtarget=x86_64-linux-gnu` on an x86_64 Debian is the same machine, but zig treats it as a
/// cross build and stops consulting the system's headers. That distinction is invisible in the
/// error it produces, which is a page of headers that are not found.
fn targetIsHost(target: std.Build.ResolvedTarget) bool {
    if (target.query.isNative()) return true;
    const t = target.result;
    return t.cpu.arch == builtin.target.cpu.arch and t.os.tag == builtin.target.os.tag and
        t.abi == builtin.target.abi;
}

/// Puts the host's own header and library directories on @p compile, for what zig does not carry.
///
/// Three of them, for two different reasons:
///
///   /usr/include                     The kernel's headers, `linux/io_uring.h` among them.
///   /usr/include/<arch>-linux-<abi>  On Debian and Ubuntu the more interesting half of libc,
///                                    `asm/errno.h` and friends, sits one level deeper under
///                                    the multiarch triple and zig's detection stops above it.
///   /usr/lib/<arch>-linux-<abi>      Where the system's libraries resolve.
///
/// A native target already has the first and the third; a *named* host target has neither,
/// which is the whole reason this function exists. A target that is not the host gets nothing:
/// its headers and libraries are not these, and pretending otherwise builds a binary against
/// one machine's libraries for another's.
fn addHostSystemPaths(
    b: *std.Build,
    compile: *std.Build.Step.Compile,
    target: std.Build.ResolvedTarget,
) void {
    const t = target.result;
    if (!targetIsHost(target) or t.os.tag != .linux) return;
    if (!std.mem.startsWith(u8, @tagName(t.abi), "gnu") and
        !std.mem.startsWith(u8, @tagName(t.abi), "musl")) return;

    const triple = b.fmt("{s}-linux-{s}", .{ @tagName(t.cpu.arch), @tagName(t.abi) });
    const named_host = !target.query.isNative();

    if (named_host) compile.addSystemIncludePath(.{ .cwd_relative = "/usr/include" });

    const multiarch_include = b.fmt("/usr/include/{s}", .{triple});
    if (std.fs.accessAbsolute(multiarch_include, .{})) |_| {
        compile.addSystemIncludePath(.{ .cwd_relative = multiarch_include });
    } else |_| {}

    if (!named_host) return;
    for ([_][]const u8{ b.fmt("/usr/lib/{s}", .{triple}), "/usr/lib", "/lib" }) |dir| {
        std.fs.accessAbsolute(dir, .{}) catch continue;
        compile.addLibraryPath(.{ .cwd_relative = dir });
    }
}

/// A libc description Debian's native detection does not produce, for the compiles this build
/// does not own.
///
/// `addMultiarchIncludeDir` above fixes the targets declared in this file. It cannot fix the
/// ones zig compiles on its own account -- libtsan, built before it can be linked -- or the
/// ones a dependency declares, and libsodium is the second kind: its sources include `errno.h`,
/// which on Debian and Ubuntu reaches `asm/errno.h` under the multiarch triple, and
/// `zig libc` reports `sys_include_dir=/usr/include` regardless.
///
/// So the detected description is taken, its `sys_include_dir` is pointed at the multiarch
/// directory, and the result is handed to every target of this build -- from where it reaches
/// the dependencies as well. `--libc <file>` on the command line wins over it, and on a host
/// without that directory nothing is generated and nothing changes.
///
/// This is what makes `-Dsanitize=thread` run at all, and AGENTS.md section 4 asks for TSan in
/// CI: a sanitizer that needs a hand-written file to start is a sanitizer that does not run.
fn nativeLibcFile(b: *std.Build, target: std.Build.ResolvedTarget) ?std.Build.LazyPath {
    const t = target.result;
    if (b.libc_file != null) return null;
    if (!targetIsHost(target) or t.os.tag != .linux) return null;
    if (!std.mem.startsWith(u8, @tagName(t.abi), "gnu") and
        !std.mem.startsWith(u8, @tagName(t.abi), "musl")) return null;

    const multiarch = b.fmt("/usr/include/{s}-linux-{s}", .{ @tagName(t.cpu.arch), @tagName(t.abi) });
    std.fs.accessAbsolute(multiarch, .{}) catch return null;

    const detected = std.process.Child.run(.{
        .allocator = b.allocator,
        .argv = &.{ b.graph.zig_exe, "libc" },
    }) catch return null;
    if (detected.term != .Exited or detected.term.Exited != 0) return null;

    var patched: std.ArrayList(u8) = .empty;
    var lines = std.mem.splitScalar(u8, detected.stdout, '\n');
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "sys_include_dir=")) {
            patched.appendSlice(b.allocator, b.fmt("sys_include_dir={s}\n", .{multiarch})) catch @panic("OOM");
        } else {
            patched.appendSlice(b.allocator, line) catch @panic("OOM");
            patched.append(b.allocator, '\n') catch @panic("OOM");
        }
    }
    return b.addWriteFiles().add("libc.txt", patched.items);
}


const sqlite_defines = [_][2][]const u8{
    // Two connections on two threads (the loop reads, the writer thread writes), each used by
    // one thread only: multi-thread mode, no mutex per connection.
    .{ "SQLITE_THREADSAFE", "2" },
    .{ "SQLITE_DQS", "0" },
    .{ "SQLITE_DEFAULT_WAL_SYNCHRONOUS", "1" },
    .{ "SQLITE_DEFAULT_MEMSTATUS", "0" },
    .{ "SQLITE_MAX_EXPR_DEPTH", "0" },
    .{ "SQLITE_LIKE_DOESNT_MATCH_BLOBS", "1" },
    .{ "SQLITE_OMIT_DEPRECATED", "1" },
    .{ "SQLITE_OMIT_LOAD_EXTENSION", "1" },
    .{ "SQLITE_OMIT_SHARED_CACHE", "1" },
    .{ "SQLITE_USE_ALLOCA", "1" },
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const libc_file = nativeLibcFile(b, target);

    const sodium = b.dependency("libsodium", .{
        .target = target,
        .optimize = optimize,
        .static = true,
        .shared = false,
    }).artifact("sodium");
    const arnm = b.dependency("arnm", .{ .target = target, .optimize = optimize }).artifact("arnm");

    // SQLite, from the amalgamation
    const sqlite_dep = b.dependency("sqlite3", .{});
    const sqlite = b.addLibrary(.{
        .name = "sqlite3",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true }),
    });
    addHostSystemPaths(b, sqlite, target);
    for (sqlite_defines) |define| sqlite.root_module.addCMacro(define[0], define[1]);
    sqlite.addCSourceFiles(.{ .root = sqlite_dep.path("."), .files = &.{"sqlite3.c"}, .flags = &.{"-std=gnu11"} });
    sqlite.installHeader(sqlite_dep.path("sqlite3.h"), "sqlite3.h");

    const pico_dep = b.dependency("picohttpparser", .{});

    const exe = b.addExecutable(.{
        .name = "server",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true }),
    });
    addHostSystemPaths(b, exe, target);
    exe.root_module.addCMacro("_GNU_SOURCE", "1");
    exe.addIncludePath(pico_dep.path("."));
    exe.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{ "main.c", "server.c", "db.c", "jwt.c" },
        .flags = &.{ "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter" },
    });
    exe.addCSourceFiles(.{ .root = pico_dep.path("."), .files = &.{"picohttpparser.c"}, .flags = &.{"-std=gnu11"} });
    exe.linkLibrary(sqlite);
    exe.linkLibrary(sodium);
    exe.linkLibrary(arnm);

    if (libc_file) |file| {
        for ([_]*std.Build.Step.Compile{ exe, sqlite, sodium, arnm }) |compile| compile.setLibCFile(file);
    }

    b.installArtifact(exe);
}
