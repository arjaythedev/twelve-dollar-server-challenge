use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=btree.c");
    println!("cargo:rerun-if-changed=sqlite/sqlite3.c");
    println!("cargo:rerun-if-changed=sqlite3.h");
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let obj = out.join("btree.o");
    let lib = out.join("libbtree.a");
    let mut cc = Command::new("cc");
    cc.args([
        "-O3",
        "-DNDEBUG",
        "-fPIC",
        "-march=native",
        "-DSQLITE_THREADSAFE=2",
        "-DSQLITE_OMIT_LOAD_EXTENSION",
        "-DSQLITE_DEFAULT_MEMSTATUS=0",
        "-I.",
        "-c",
        "btree.c",
        "-o",
    ]);
    cc.arg(&obj);
    let status = cc.status().expect("run cc");
    assert!(status.success(), "cc failed");
    let status = Command::new("ar")
        .args(["crs"])
        .arg(&lib)
        .arg(&obj)
        .status()
        .expect("run ar");
    assert!(status.success(), "ar failed");
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=btree");
    println!("cargo:rustc-link-lib=pthread");
    println!("cargo:rustc-link-lib=dl");
    println!("cargo:rustc-link-lib=m");
}
