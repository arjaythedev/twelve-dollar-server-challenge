fn main() {
    println!("cargo:rerun-if-changed=vendor/sqlite");

    cc::Build::new()
        .file("vendor/sqlite/sqlite3.c")
        // single thread: no mutexes around every call
        .define("SQLITE_THREADSAFE", "0")
        // no global malloc accounting
        .define("SQLITE_DEFAULT_MEMSTATUS", "0")
        .define("SQLITE_DEFAULT_WAL_SYNCHRONOUS", "1")
        .define("SQLITE_DQS", "0")
        .define("SQLITE_LIKE_DOESNT_MATCH_BLOBS", None)
        .define("SQLITE_MAX_EXPR_DEPTH", "0")
        .define("SQLITE_OMIT_DEPRECATED", None)
        .define("SQLITE_OMIT_PROGRESS_CALLBACK", None)
        .define("SQLITE_OMIT_SHARED_CACHE", None)
        // also drops the need for -ldl
        .define("SQLITE_OMIT_LOAD_EXTENSION", None)
        .define("SQLITE_USE_ALLOCA", None)
        .warnings(false)
        .compile("sqlite3");
}
