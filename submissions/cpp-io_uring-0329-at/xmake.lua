add_rules("mode.debug", "mode.release")

set_languages("c11", "c++23")

set_optimize("fastest")

set_toolchains("gcc")

set_policy("build.optimization.lto", true)

-- PGO：PGO_MODE=generate 出一版带计数的二进制，训练后再用 PGO_MODE=use 重编。
-- 训练数据放独立目录，两遍的构建目录一致，gcda 才能对上（见 build.sh）。
local pgo_mode = os.getenv("PGO_MODE") or "off"
local pgo_dir = os.getenv("PGO_DIR") or "/tmp/pgo"

target("server")
    set_kind("binary")
    set_targetdir(".")

    add_includedirs(".", "src", "vendor")

    add_files("main.cpp", "src/*.cpp", "src/shim.c")

    add_defines(
        "_GNU_SOURCE",
        "SQLITE_THREADSAFE=2",
        "SQLITE_DEFAULT_MEMSTATUS=0",
        "SQLITE_DEFAULT_WAL_SYNCHRONOUS=1",
        "SQLITE_DQS=0",
        "SQLITE_LIKE_DOESNT_MATCH_BLOBS",
        "SQLITE_MAX_EXPR_DEPTH=0",
        "SQLITE_USE_ALLOCA",
        "SQLITE_OMIT_DEPRECATED",
        "SQLITE_OMIT_PROGRESS_CALLBACK",
        "SQLITE_OMIT_SHARED_CACHE",
        "SQLITE_OMIT_LOAD_EXTENSION",
        "SQLITE_OMIT_AUTOINIT",
        "SQLITE_OMIT_UTF16",
        "SQLITE_OMIT_TRACE",
        "SQLITE_OMIT_COMPLETE",
        "SQLITE_OMIT_GET_TABLE",
        "SQLITE_OMIT_TCL_VARIABLE",
        "HAVE_FDATASYNC=1",
        "HAVE_USLEEP=1")

    if pgo_mode == "generate" then
        add_cflags("-fprofile-generate=" .. pgo_dir)
        add_cxxflags("-fprofile-generate=" .. pgo_dir)
        add_ldflags("-fprofile-generate=" .. pgo_dir)
    elseif pgo_mode == "use" then
        add_cflags("-fprofile-use=" .. pgo_dir .. " -fprofile-correction -Wno-missing-profile")
        add_cxxflags("-fprofile-use=" .. pgo_dir .. " -fprofile-correction -Wno-missing-profile")
    end

    if is_plat("linux") then
        add_cflags("-march=native", "-fno-plt", "-fno-semantic-interposition", "-falign-functions=32", "-falign-loops=32", "-fipa-pta", "-mprefer-vector-width=256")
        add_cxxflags("-march=native", "-fno-plt", "-fno-semantic-interposition", "-falign-functions=32", "-falign-loops=32", "-fipa-pta", "-mprefer-vector-width=256")
        add_cxxflags("-Wformat=2")
        add_ldflags("-flto-partition=one")
        if os.isfile("/usr/bin/mold") then
            add_ldflags("-fuse-ld=mold")
        end
        add_syslinks("uring", "pthread", "m")
    end
