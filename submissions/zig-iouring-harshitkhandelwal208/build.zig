const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const translate = b.addTranslateC(.{
        .root_source_file = b.path("deps/sqlite-amalgamation-3540100/sqlite3.h"),
        .target = target,
        .optimize = optimize,
    });
    const c_mod = translate.createModule();

    const exe = b.addExecutable(.{
        .name = "server",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .single_threaded = false,
            .strip = (optimize != .debug),
        }),
    });

    exe.root_module.addImport("c", c_mod);

    const sqlite_flags = [_][]const u8{
        "-O3",
        "-DSQLITE_THREADSAFE=2",
        "-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1",
        "-DSQLITE_DEFAULT_MEMSTATUS=0",
        "-DSQLITE_DQS=0",
        "-DSQLITE_OMIT_DEPRECATED",
        "-DSQLITE_OMIT_SHARED_CACHE",
        "-DSQLITE_OMIT_PROGRESS_CALLBACK",
        "-DSQLITE_OMIT_LOAD_EXTENSION",
        "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS",
        "-DSQLITE_MAX_EXPR_DEPTH=0",
        "-DSQLITE_USE_ALLOCA",
        "-DHAVE_USLEEP=1",
        "-DHAVE_FDATASYNC=1",
        "-DSQLITE_DISABLE_DIRSYNC",
        "-DSQLITE_OMIT_AUTHORIZATION",
        "-DSQLITE_OMIT_EXPLAIN",
    };

    exe.root_module.addIncludePath(b.path("deps/sqlite-amalgamation-3540100"));
    exe.root_module.addCSourceFile(.{
        .file = b.path("src/shim.c"),
        .flags = &sqlite_flags,
    });
    exe.root_module.link_libc = true;

    // Output directly to bin/server
    const install_exe = b.addInstallArtifact(exe, .{
        .dest_dir = .{ .override = .{ .custom = "../bin" } },
    });
    b.getInstallStep().dependOn(&install_exe.step);
}
