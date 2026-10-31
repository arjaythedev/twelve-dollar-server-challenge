fn main() {
    println!("cargo:rerun-if-changed=native/sqlite.c");
    println!("cargo:rerun-if-changed=vendor/sqlite3.c");
    let mut build = cc::Build::new();
    build.file("native/sqlite.c").opt_level(3)
        .define("SQLITE_THREADSAFE", "2")
        .define("SQLITE_DEFAULT_MEMSTATUS", "0")
        .define("SQLITE_DQS", "0")
        .define("SQLITE_OMIT_LOAD_EXTENSION", None)
        .define("SQLITE_OMIT_DEPRECATED", None)
        .define("SQLITE_OMIT_SHARED_CACHE", None)
        .define("SQLITE_OMIT_PROGRESS_CALLBACK", None)
        .define("SQLITE_LIKE_DOESNT_MATCH_BLOBS", None)
        .define("SQLITE_USE_ALLOCA", None);
    if std::env::var("HOST").unwrap() == std::env::var("TARGET").unwrap() {
        build.flag_if_supported("-march=native");
    }
    if std::env::var("CARGO_CFG_TARGET_OS").unwrap() == "linux" {
        build.define("HAVE_FDATASYNC", "1");
    }
    build.compile("sqlite3");
    println!("cargo:rustc-link-lib=m");
}
