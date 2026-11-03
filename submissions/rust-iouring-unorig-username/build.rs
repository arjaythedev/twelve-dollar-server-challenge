// Links the SQLite amalgamation that build.sh compiled into deps/libsqlite3.a (pinned 3.54.1).
fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo:rustc-link-search=native={dir}/deps");
    println!("cargo:rustc-link-lib=static=sqlite3");
    println!("cargo:rerun-if-changed=deps/libsqlite3.a");
}
