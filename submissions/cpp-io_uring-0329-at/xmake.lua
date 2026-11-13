add_rules("mode.debug", "mode.release")

set_languages("c11", "c++23")

set_optimize("fastest")

set_toolchains("gcc")

set_policy("build.optimization.lto", true)

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

    if is_plat("linux") then
        add_cflags("-march=native", "-fno-plt", "-fno-semantic-interposition")
        add_cxxflags("-march=native", "-fno-plt", "-fno-semantic-interposition")
        add_syslinks("uring", "pthread", "m")
    end
