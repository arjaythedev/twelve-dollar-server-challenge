pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.option(
        std.builtin.Optimize,
        "optimize",
        "Optimization mode (default: fast)",
    ) orelse .fast;

    // zig-staticio lets us implement our own Io without having to implement the full interface
    const std_with_staticio = b.dependency("patch", .{
        .std_patches = &[_]std.Build.LazyPath{
            b.dependency("staticio", .{}).namedLazyPath("std.patch"),
        },
    }).module("std");

    const exe = b.addExecutable(.{
        .name = "server",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            // UringIo parks a fiber's stack from its stack pointer up, so
            // nothing may live below it.
            .red_zone = false,
            .imports = &.{.{ .name = "std", .module = std_with_staticio }},
        }),
    });

    const sqlite = b.dependency("sqlite", .{});
    exe.root_module.addIncludePath(sqlite.path(""));
    exe.root_module.addCSourceFile(.{
        .file = sqlite.path("sqlite3.c"),
        .flags = &.{
            // multi-thread mode: each connection is used by one thread (the
            // event loop or the checkpointer), so no per-connection mutexes
            "-DSQLITE_THREADSAFE=2",
            "-DSQLITE_DEFAULT_MEMSTATUS=0",
            "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1",
            "-DSQLITE_DQS=0",
            "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS",
            "-DSQLITE_MAX_EXPR_DEPTH=0",
            "-DSQLITE_OMIT_DEPRECATED",
            "-DSQLITE_OMIT_LOAD_EXTENSION",
            "-DSQLITE_OMIT_PROGRESS_CALLBACK",
            "-DSQLITE_OMIT_SHARED_CACHE",
            "-DSQLITE_OMIT_AUTOINIT",
            "-DSQLITE_OMIT_DECLTYPE",
            "-DSQLITE_USE_ALLOCA",
            "-DSQLITE_TEMP_STORE=3",
            "-DHAVE_USLEEP=1",
        },
    });
    b.installArtifact(exe);

    const loadgen = b.addExecutable(.{
        .name = "loadgen",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/loadgen.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .red_zone = false,
            .imports = &.{.{ .name = "std", .module = std_with_staticio }},
        }),
    });
    b.step("loadgen", "Build the load generator (bench/load.js as fibers)").dependOn(&b.addInstallArtifact(loadgen, .{}).step);

    const run = b.addRunArtifact(exe);
    run.step.dependOn(b.getInstallStep());
    b.step("run", "Run the server").dependOn(&run.step);

    const tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/tests.zig"),
        .target = target,
        .optimize = optimize,
    }) });
    b.step("test", "Run unit tests").dependOn(&b.addRunArtifact(tests).step);
}

const std = @import("std");
