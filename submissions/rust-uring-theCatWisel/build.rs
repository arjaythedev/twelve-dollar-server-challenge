use std::env;
use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let vendor_dir = manifest_dir.join("vendor");
    println!("cargo:rustc-link-search=native={}", vendor_dir.display());
    println!("cargo:rustc-link-lib=static=sqlite_engine");
    println!("cargo:rustc-link-lib=pthread");
    println!("cargo:rustc-link-lib=m");
    println!("cargo:rustc-link-lib=dl");
    if env::var("PGO_STAGE").unwrap_or_default() == "gen" {
        println!("cargo:rustc-link-lib=gcov");
    }
    println!("cargo:rerun-if-env-changed=PGO_STAGE");
    println!("cargo:rerun-if-changed=vendor/libsqlite_engine.a");
}
