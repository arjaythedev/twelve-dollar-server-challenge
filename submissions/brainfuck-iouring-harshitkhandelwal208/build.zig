const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const translate = b.addTranslateC(.{
        .root_source_file = b.path("src/c.h"),
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
    exe.root_module.linkSystemLibrary("sqlite3", .{});
    exe.root_module.addObjectFile(b.path("build/server_bf.o"));
    exe.root_module.link_libc = true;

    // Output directly to bin/server
    const install_exe = b.addInstallArtifact(exe, .{
        .dest_dir = .{ .override = .{ .custom = "../bin" } },
    });
    b.getInstallStep().dependOn(&install_exe.step);
}
