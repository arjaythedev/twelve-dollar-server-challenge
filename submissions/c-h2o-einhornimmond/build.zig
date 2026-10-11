//! Builds bin/server: h2o (evloop), SQLite, libsodium and arnm, all from pinned sources, with
//! LibreSSL and zlib underneath h2o. The h2o source list and the Debian/Ubuntu libc handling are
//! the ones gradido-unicorn's fast-servers build uses.
const std = @import("std");
const builtin = @import("builtin");

/// True when @p target names the machine this build runs on.
///
/// Either by being native, or by spelling the host's own triple out --
/// `-Dtarget=x86_64-linux-gnu` on an x86_64 Debian is the same machine, but zig treats it as a
/// cross build and stops consulting the system's headers. That distinction is invisible in the
/// error it produces, which is 78 lines of `'openssl/ssl.h' file not found`.
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
///   /usr/include                     OpenSSL and zlib, which h2o includes and which no
///                                    dependency of this build provides.
///   /usr/include/<arch>-linux-<abi>  On Debian and Ubuntu the more interesting half of libc,
///                                    `asm/errno.h` and friends, sits one level deeper under
///                                    the multiarch triple and zig's detection stops above it.
///   /usr/lib/<arch>-linux-<abi>      Where the -l flags for ssl, crypto and z resolve.
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


/// The source list of libh2o-evloop, copied from LIB_SOURCE_FILES in h2o's CMakeLists.txt.
/// QUIC and HTTP/3 come along for the ride: h2o's core refers to them whether or not anyone
/// intends to speak them.
const h2o_sources = [_][]const u8{
    "deps/cloexec/cloexec.c",
    "deps/hiredis/async.c",
    "deps/hiredis/hiredis.c",
    "deps/hiredis/net.c",
    "deps/hiredis/read.c",
    "deps/hiredis/sds.c",
    "deps/hiredis/alloc.c",
    "deps/libgkc/gkc.c",
    "deps/libyrmcds/close.c",
    "deps/libyrmcds/connect.c",
    "deps/libyrmcds/recv.c",
    "deps/libyrmcds/send.c",
    "deps/libyrmcds/send_text.c",
    "deps/libyrmcds/socket.c",
    "deps/libyrmcds/strerror.c",
    "deps/libyrmcds/text_mode.c",
    "deps/picohttpparser/picohttpparser.c",
    "deps/picotls/deps/cifra/src/blockwise.c",
    "deps/picotls/deps/cifra/src/chash.c",
    "deps/picotls/deps/cifra/src/curve25519.c",
    "deps/picotls/deps/cifra/src/drbg.c",
    "deps/picotls/deps/cifra/src/hmac.c",
    "deps/picotls/deps/cifra/src/sha256.c",
    "deps/picotls/lib/hpke.c",
    "deps/picotls/lib/pembase64.c",
    "deps/picotls/lib/picotls.c",
    "deps/picotls/lib/openssl.c",
    "deps/picotls/lib/cifra/random.c",
    "deps/picotls/lib/cifra/x25519.c",
    "deps/quicly/lib/cc-cubic.c",
    "deps/quicly/lib/cc-pico.c",
    "deps/quicly/lib/cc-reno.c",
    "deps/quicly/lib/defaults.c",
    "deps/quicly/lib/frame.c",
    "deps/quicly/lib/local_cid.c",
    "deps/quicly/lib/loss.c",
    "deps/quicly/lib/quicly.c",
    "deps/quicly/lib/ranges.c",
    "deps/quicly/lib/rate.c",
    "deps/quicly/lib/recvstate.c",
    "deps/quicly/lib/remote_cid.c",
    "deps/quicly/lib/sendstate.c",
    "deps/quicly/lib/sentmap.c",
    "deps/quicly/lib/streambuf.c",

    "lib/common/cache.c",
    "lib/common/file.c",
    "lib/common/filecache.c",
    "lib/common/hostinfo.c",
    "lib/common/http1client.c",
    "lib/common/http2client.c",
    "lib/common/http3client.c",
    "lib/common/httpclient.c",
    "lib/common/memcached.c",
    "lib/common/memory.c",
    "lib/common/multithread.c",
    "lib/common/redis.c",
    "lib/common/serverutil.c",
    "lib/common/socket.c",
    "lib/common/socketpool.c",
    "lib/common/string.c",
    "lib/common/rand.c",
    "lib/common/time.c",
    "lib/common/timerwheel.c",
    "lib/common/token.c",
    "lib/common/url.c",
    "lib/common/balancer/roundrobin.c",
    "lib/common/balancer/least_conn.c",
    "lib/common/absprio.c",

    "lib/core/config.c",
    "lib/core/configurator.c",
    "lib/core/context.c",
    "lib/core/headers.c",
    "lib/core/logconf.c",
    "lib/core/pipe_sender.c",
    "lib/core/proxy.c",
    "lib/core/request.c",
    "lib/core/util.c",

    "lib/handler/access_log.c",
    "lib/handler/compress.c",
    "lib/handler/compress/gzip.c",
    "lib/handler/errordoc.c",
    "lib/handler/expires.c",
    "lib/handler/fastcgi.c",
    "lib/handler/file.c",
    "lib/handler/h2olog.c",
    "lib/handler/headers.c",
    "lib/handler/headers_util.c",
    "lib/handler/http2_debug_state.c",
    "lib/handler/mimemap.c",
    "lib/handler/proxy.c",
    "lib/handler/connect.c",
    "lib/handler/redirect.c",
    "lib/handler/reproxy.c",
    "lib/handler/throttle_resp.c",
    "lib/handler/self_trace.c",
    "lib/handler/server_timing.c",
    "lib/handler/status.c",
    "lib/handler/status/events.c",
    "lib/handler/status/memory.c",
    "lib/handler/status/requests.c",
    "lib/handler/status/ssl.c",
    "lib/handler/status/durations.c",
    "lib/handler/configurator/access_log.c",
    "lib/handler/configurator/compress.c",
    "lib/handler/configurator/errordoc.c",
    "lib/handler/configurator/expires.c",
    "lib/handler/configurator/fastcgi.c",
    "lib/handler/configurator/file.c",
    "lib/handler/configurator/h2olog.c",
    "lib/handler/configurator/headers.c",
    "lib/handler/configurator/headers_util.c",
    "lib/handler/configurator/http2_debug_state.c",
    "lib/handler/configurator/proxy.c",
    "lib/handler/configurator/redirect.c",
    "lib/handler/configurator/reproxy.c",
    "lib/handler/configurator/throttle_resp.c",
    "lib/handler/configurator/self_trace.c",
    "lib/handler/configurator/server_timing.c",
    "lib/handler/configurator/status.c",

    "lib/http1.c",

    "lib/http2/cache_digests.c",
    "lib/http2/casper.c",
    "lib/http2/connection.c",
    "lib/http2/frame.c",
    "lib/http2/hpack.c",
    "lib/http2/scheduler.c",
    "lib/http2/stream.c",
    "lib/http2/http2_debug_state.c",

    "lib/http3/frame.c",
    "lib/http3/qpack.c",
    "lib/http3/common.c",
    "lib/http3/server.c",
};

/// libyaml. h2o's configurator reaches for yoml, yoml reaches for yaml, and the linker
/// eventually asks who is going to pay for all this.
const yaml_sources = [_][]const u8{
    "deps/yaml/src/api.c",
    "deps/yaml/src/dumper.c",
    "deps/yaml/src/emitter.c",
    "deps/yaml/src/loader.c",
    "deps/yaml/src/parser.c",
    "deps/yaml/src/reader.c",
    "deps/yaml/src/scanner.c",
    "deps/yaml/src/writer.c",
};

const h2o_include_dirs = [_][]const u8{
    "include",
    "deps/cloexec",
    "deps/golombset",
    "deps/hiredis",
    "deps/libgkc",
    "deps/libyrmcds",
    "deps/klib",
    "deps/neverbleed",
    "deps/picohttpparser",
    "deps/picotest",
    "deps/picotls/deps/cifra/src/ext",
    "deps/picotls/deps/cifra/src",
    "deps/picotls/deps/micro-ecc",
    "deps/picotls/include",
    "deps/quicly/include",
    "deps/yaml/include",
    "deps/yoml",
};

/// h2o's own sources, which are not ours to keep clean under -Wall -Wextra.
const h2o_c_flags = [_][]const u8{
    "-std=gnu99",
    "-Wno-unused-value",
    "-Wno-unused-function",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-implicit-fallthrough",
    "-Wno-sign-compare",
    "-Wno-missing-field-initializers",
    "-Wno-deprecated-declarations",
};

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

    const libressl_dep = b.dependency("libressl", .{ .target = target, .optimize = optimize });
    const ssl = libressl_dep.artifact("ssl");
    const zlib = b.dependency("zlib", .{ .target = target, .optimize = optimize }).artifact("z");
    const sodium = b.dependency("libsodium", .{
        .target = target,
        .optimize = optimize,
        .static = true,
        .shared = false,
    }).artifact("sodium");
    const arnm = b.dependency("arnm", .{ .target = target, .optimize = optimize }).artifact("arnm");

    // libh2o-evloop
    const h2o_dep = b.dependency("h2o", .{});
    const h2o = b.addLibrary(.{
        .name = "h2o-evloop",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true }),
    });
    addHostSystemPaths(b, h2o, target);
    h2o.linkLibrary(ssl);
    h2o.linkLibrary(zlib);
    for (h2o_include_dirs) |dir| h2o.addIncludePath(h2o_dep.path(dir));
    h2o.root_module.addCMacro("H2O_USE_LIBUV", "0");
    h2o.root_module.addCMacro("_GNU_SOURCE", "1");
    h2o.root_module.addCMacro("H2O_ROOT", "\"/usr/local\"");
    h2o.root_module.addCMacro("H2O_CONFIG_PATH", "\"/usr/local/etc/h2o.conf\"");
    h2o.addCSourceFiles(.{ .root = h2o_dep.path("."), .files = &h2o_sources, .flags = &h2o_c_flags });
    h2o.addCSourceFiles(.{ .root = h2o_dep.path("."), .files = &yaml_sources, .flags = &h2o_c_flags });

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

    const exe = b.addExecutable(.{
        .name = "server",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true }),
    });
    addHostSystemPaths(b, exe, target);
    exe.root_module.addCMacro("_GNU_SOURCE", "1");
    exe.root_module.addCMacro("H2O_USE_LIBUV", "0");
    for (h2o_include_dirs) |dir| exe.addIncludePath(h2o_dep.path(dir));
    exe.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{ "main.c", "server.c", "db.c", "jwt.c" },
        .flags = &.{ "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter" },
    });
    exe.linkLibrary(h2o);
    exe.linkLibrary(ssl);
    exe.linkLibrary(zlib);
    exe.linkLibrary(sqlite);
    exe.linkLibrary(sodium);
    exe.linkLibrary(arnm);

    if (libc_file) |file| {
        for ([_]*std.Build.Step.Compile{
            exe,                              h2o,                              sqlite, sodium, arnm, zlib, ssl,
            libressl_dep.artifact("crypto"), libressl_dep.artifact("tls"),
        }) |compile| compile.setLibCFile(file);
    }

    b.installArtifact(exe);
}
