// Compiles csrc/shim.c, which #includes the pinned SQLite amalgamation that build.sh fetched
// into vendor/.
fn main() {
    println!("cargo:rerun-if-changed=csrc/shim.c");
    let mut b = cc::Build::new();
    b.file("csrc/shim.c")
        .include("vendor")
        .opt_level(3)
        .flag_if_supported("-march=native")
        .flag("-fno-plt")
        .warnings(false);
    for d in [
        "SQLITE_THREADSAFE=0",
        "SQLITE_DEFAULT_MEMSTATUS=0",
        "SQLITE_DEFAULT_WAL_SYNCHRONOUS=1",
        "SQLITE_DQS=0",
        "SQLITE_LIKE_DOESNT_MATCH_BLOBS",
        "SQLITE_MAX_EXPR_DEPTH=0",
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
        "SQLITE_USE_ALLOCA",
        "HAVE_FDATASYNC=1",
        "HAVE_USLEEP=1",
    ] {
        match d.split_once('=') {
            Some((k, v)) => b.define(k, v),
            None => b.define(d, None),
        };
    }
    b.compile("shim");
    println!("cargo:rustc-link-lib=m");
}
