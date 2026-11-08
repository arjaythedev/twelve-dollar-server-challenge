//! Cargo build script for twelve-loop: checks the vendored SQLite source id, hashes the source set,
//! compiles the one C translation unit into a static library and exports the build facts the
//! startup line prints.

use std::env;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use sha2::{Digest, Sha256};

/// `SQLITE_SOURCE_ID` of SQLite 3.53.4's amalgamation, the only one the C modules may compile against.
const SOURCE_ID: &str =
    "2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc";

/// SQLite compile options, in the order the compiler receives them.
const SQLITE_DEFINES: [&str; 10] = [
    "-DSQLITE_THREADSAFE=0",
    "-DSQLITE_DEFAULT_MEMSTATUS=0",
    "-DSQLITE_DQS=0",
    "-DHAVE_FDATASYNC=1",
    "-DSQLITE_ENABLE_QPSG",
    "-DSQLITE_OMIT_LOAD_EXTENSION",
    "-DSQLITE_OMIT_DEPRECATED",
    "-DSQLITE_OMIT_SHARED_CACHE",
    "-DSQLITE_OMIT_PROGRESS_CALLBACK",
    "-DSQLITE_LIKE_DOESNT_MATCH_BLOBS",
];

/// The compiler when CC is unset: GCC by full path, never a PATH lookup.
const DEFAULT_COMPILER: &str = "/usr/bin/gcc";

/// The archiver, by full path.
const ARCHIVER: &str = "/usr/bin/ar";

/// The whole environment of every child: its environment is cleared first, then this PATH alone is set.
const CHILD_PATH: &str = "/usr/bin:/bin";

/// The per-frame byte budget of our own C, passed as -Wframe-larger-than. Basis: the largest
/// automatic arrays are twelve_feed's 20 fragments and 20 ids (960 bytes, readers.c), and 16 KiB leaves
/// room for SQLite callees inlined at -O3 while one frame stays a sixteenth of the checkpointer fiber's
/// 262,144-byte stack.
const STACK_BUFFER_MAX_BYTES: u32 = 16_384;

/// The C warning set up to -Wframe-larger-than, in its order, with every warning an error. The unity
/// unit's compile leaves out -Wnull-dereference: GCC diagnoses it in the optimizer after inlining, so its
/// findings land in the vendored sqlite3.c, which -isystem cannot scope.
const WARNING_FLAGS: [&str; 18] = [
    "-pedantic-errors",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wconversion",
    "-Wsign-conversion",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-Wold-style-definition",
    "-Wvla",
    "-Wundef",
    "-Wformat=2",
    "-Wcast-qual",
    "-Wcast-align",
    "-Wimplicit-fallthrough",
    "-Wswitch-enum",
    "-Wdouble-promotion",
];

/// The GCC-only warnings, after -Wframe-larger-than.
const GCC_WARNING_FLAGS: [&str; 4] = [
    "-Wcast-align=strict",
    "-Wjump-misses-init",
    "-Walloca",
    "-Wwrite-strings",
];

/// The release hardening flags, which the safe build adds outside the sanitizer test profile.
const HARDENING_FLAGS: [&str; 8] = [
    "-fno-strict-overflow",
    "-fno-strict-aliasing",
    "-fno-delete-null-pointer-checks",
    "-ftrivial-auto-var-init=zero",
    "-U_FORTIFY_SOURCE",
    "-D_FORTIFY_SOURCE=3",
    "-fstack-protector-strong",
    "-fstack-clash-protection",
];

/// The sanitizer kinds GCC builds, each selected by a -Zsanitizer=<kind> of the effective Rust flags.
const SANITIZER_KINDS: [&str; 4] = ["address", "leak", "thread", "hwaddress"];

/// First bytes of the source-hash stream; one record per file follows.
const HASH_PREFIX: &[u8] = b"twelve-source\0";

/// The one reusable buffer that streams each source file into the hasher: 8 KiB, std::io's default
/// buffer size.
const HASH_BUFFER_BYTES: usize = 8 * 1024;

/// Files whose change reruns this script; `src` is tracked as a whole directory.
const RERUN_PATHS: [&str; 5] = [
    "build.rs",
    "Cargo.lock",
    "src",
    "vendor/sqlite3.c",
    "vendor/sqlite3.h",
];

/// Environment inputs whose change reruns this script.
const RERUN_VARS: [&str; 5] = [
    "TWELVE_C_TARGET",
    "TWELVE_PGO",
    "TWELVE_PGO_DIR",
    "CC",
    "CARGO_ENCODED_RUSTFLAGS",
];

/// The cfg flags the crate's configurations pass in RUSTFLAGS, declared for rustc's unexpected_cfgs lint.
const CFG_FLAGS: [&str; 3] = ["twelve_safe", "twelve_small_limits", "twelve_fault_inject"];

/// The failing pipeline stage and its detail, reported once by `main`. An I/O failure's detail names its
/// operation and path or program and keeps the io::Error as its source; a contract refusal has a fixed
/// detail and no source.
struct BuildFailure {
    stage: &'static str,
    detail: String,
    source: Option<io::Error>,
}

impl BuildFailure {
    fn refusal(stage: &'static str, detail: impl Into<String>) -> Self {
        Self {
            stage,
            detail: detail.into(),
            source: None,
        }
    }

    /// `<operation> <path>`; the path is shown for the reader only and never compared or parsed.
    fn io(stage: &'static str, operation: &str, path: &Path, error: io::Error) -> Self {
        Self {
            stage,
            detail: format!("{operation} {}", path.display()),
            source: Some(error),
        }
    }
}

/// The validated, immutable build configuration.
struct Config {
    manifest_dir: PathBuf,
    /// Valid UTF-8 without CR or LF, since it is printed in a Cargo directive.
    out_dir: String,
    c_target: String,
    compiler: PathBuf,
    /// Present only for a profile-use build; an existing canonical absolute directory, printable like `out_dir`.
    pgo_dir: Option<String>,
    safe: bool,
    fault_inject: bool,
    /// The sanitizer kinds of the effective Rust flags, in order; empty outside the sanitizer test profile.
    sanitizers: Vec<String>,
    rust_flags: Vec<String>,
}

fn main() {
    if let Err(failure) = run() {
        let detail = match failure.source {
            Some(source) => format!("{}: {source}", failure.detail),
            None => failure.detail,
        };
        // The exit status reports the failure whether or not stderr takes the line.
        let _ = writeln!(
            io::stderr().lock(),
            "build.rs: failed {}: {}",
            failure.stage,
            detail.escape_debug()
        );
        std::process::exit(1);
    }
}

fn run() -> Result<(), BuildFailure> {
    let config = read_config()?;
    emit(&rerun_directives(&config))?;
    check_source_id(&config.manifest_dir)?;
    let digest = source_hash(&config.manifest_dir)?;
    let object = compile_unit(&config)?;
    archive_unit(&config, &object)?;
    emit(&build_facts(&config, &digest))
}

fn read_config() -> Result<Config, BuildFailure> {
    let refuse = |detail: &str| BuildFailure::refusal("config", detail);

    let host = unicode_var("HOST");
    let target = unicode_var("TARGET");
    let target_os = unicode_var("CARGO_CFG_TARGET_OS");
    let target_arch = unicode_var("CARGO_CFG_TARGET_ARCH");
    let unsupported = matches!(&target_os, Some(os) if os != "linux")
        || matches!(&target_arch, Some(arch) if arch != "x86_64")
        || matches!((&host, &target), (Some(host), Some(target)) if host != target);
    if unsupported {
        return Err(refuse("unsupported native target"));
    }

    let manifest_dir = directive_var("CARGO_MANIFEST_DIR");
    let out_dir = directive_var("OUT_DIR");
    let profile = unicode_var("PROFILE");
    let rust_flags = encoded_rust_flags();
    let (
        Some(_),
        Some(_),
        Some(_),
        Some(_),
        Some(manifest_dir),
        Some(out_dir),
        Some(profile),
        Some(rust_flags),
    ) = (
        host,
        target,
        target_os,
        target_arch,
        manifest_dir,
        out_dir,
        profile,
        rust_flags,
    )
    else {
        return Err(refuse("invalid Cargo environment"));
    };

    let c_target = match env::var_os("TWELVE_C_TARGET") {
        None => String::from("native"),
        Some(value) => match value.into_string() {
            Ok(token) if is_target_token(&token) => token,
            _ => return Err(refuse("invalid TWELVE_C_TARGET")),
        },
    };

    let compiler = match env::var_os("CC") {
        None => PathBuf::from(DEFAULT_COMPILER),
        Some(value) if !value.is_empty() => PathBuf::from(value),
        Some(_) => return Err(refuse("invalid CC")),
    };

    let pgo = match env::var_os("TWELVE_PGO") {
        None => false,
        Some(value) if value == "0" => false,
        Some(value) if value == "1" => true,
        Some(_) => return Err(refuse("invalid TWELVE_PGO")),
    };

    // Cargo sets CARGO_CFG_<cfg> for each --cfg the effective Rust flags carry.
    let release = profile == "release";
    let safe = env::var_os("CARGO_CFG_TWELVE_SAFE").is_some();
    let small_limits = env::var_os("CARGO_CFG_TWELVE_SMALL_LIMITS").is_some();
    let fault_inject = env::var_os("CARGO_CFG_TWELVE_FAULT_INJECT").is_some();
    if release && (small_limits || fault_inject) {
        return Err(refuse("test configuration in release"));
    }
    if pgo && !release {
        return Err(refuse("PGO requires release"));
    }
    let sanitizers = sanitizer_kinds(&rust_flags);
    if pgo && !sanitizers.is_empty() {
        return Err(refuse("PGO with sanitizer"));
    }

    let pgo_dir = match (pgo, env::var_os("TWELVE_PGO_DIR")) {
        (false, None) => None,
        (true, Some(dir)) => {
            Some(canonical_profile_dir(dir).ok_or_else(|| refuse("invalid TWELVE_PGO_DIR"))?)
        }
        _ => return Err(refuse("invalid TWELVE_PGO_DIR")),
    };

    if !sanitizers
        .iter()
        .all(|kind| SANITIZER_KINDS.contains(&kind.as_str()))
    {
        return Err(refuse("unsupported sanitizer"));
    }

    Ok(Config {
        manifest_dir: PathBuf::from(manifest_dir),
        out_dir,
        c_target,
        compiler,
        pgo_dir,
        safe,
        fault_inject,
        sanitizers,
        rust_flags,
    })
}

/// A variable's value when it is present and valid Unicode.
fn unicode_var(name: &str) -> Option<String> {
    env::var_os(name).and_then(|value| value.into_string().ok())
}

/// A path variable that may appear in a Cargo directive: valid UTF-8 and free of CR and LF.
fn directive_var(name: &str) -> Option<String> {
    unicode_var(name).filter(|value| !value.contains(['\r', '\n']))
}

/// Cargo's encoded Rust flags as their argument list; unset or empty is no argument. `None` when the
/// value is not valid Unicode.
fn encoded_rust_flags() -> Option<Vec<String>> {
    match env::var_os("CARGO_ENCODED_RUSTFLAGS") {
        None => Some(Vec::new()),
        Some(value) => {
            let text = value.into_string().ok()?;
            if text.is_empty() {
                Some(Vec::new())
            } else {
                Some(text.split('\u{1f}').map(str::to_owned).collect())
            }
        }
    }
}

/// The kind of each -Zsanitizer=<kind> in the Rust flags, written as one argument or as -Z followed by
/// sanitizer=<kind>, in order.
fn sanitizer_kinds(flags: &[String]) -> Vec<String> {
    let mut kinds = Vec::new();
    let mut flags = flags.iter();
    while let Some(flag) = flags.next() {
        let option = if flag == "-Z" {
            flags.next().map(String::as_str)
        } else {
            flag.strip_prefix("-Z")
        };
        if let Some(kind) = option.and_then(|text| text.strip_prefix("sanitizer=")) {
            kinds.push(kind.to_owned());
        }
    }
    kinds
}

fn is_target_token(token: &str) -> bool {
    !token.is_empty()
        && token
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

/// The profile directory when it is printable, absolute, existing and already canonical, byte for byte.
fn canonical_profile_dir(dir: OsString) -> Option<String> {
    let text = dir.into_string().ok()?;
    if text.is_empty() || text.contains(['\r', '\n']) {
        return None;
    }
    let path = Path::new(&text);
    if !path.is_absolute() {
        return None;
    }
    let canonical = fs::canonicalize(path).ok()?;
    if canonical.as_os_str() != OsStr::new(&text) || !canonical.is_dir() {
        return None;
    }
    Some(text)
}

/// Requires a regular vendor/sqlite3.h, then the pinned source id as the first
/// `#define SQLITE_SOURCE_ID` line of vendor/sqlite3.c.
fn check_source_id(manifest_dir: &Path) -> Result<(), BuildFailure> {
    let header = manifest_dir.join("vendor/sqlite3.h");
    let header_meta = fs::metadata(&header)
        .map_err(|error| BuildFailure::io("source-id", "metadata", &header, error))?;
    if !header_meta.is_file() {
        return Err(BuildFailure::refusal(
            "source-id",
            "vendor/sqlite3.h required",
        ));
    }
    File::open(&header).map_err(|error| BuildFailure::io("source-id", "open", &header, error))?;

    let refused = || BuildFailure::refusal("source-id", "SQLite 3.53.4 source id required");
    let source = manifest_dir.join("vendor/sqlite3.c");
    let file = File::open(&source)
        .map_err(|error| BuildFailure::io("source-id", "open", &source, error))?;
    let mut reader = BufReader::new(file);
    let mut line = Vec::new();
    let mut first = true;
    loop {
        line.clear();
        let read = reader
            .read_until(b'\n', &mut line)
            .map_err(|error| BuildFailure::io("source-id", "read", &source, error))?;
        if read == 0 {
            return Err(refused());
        }
        if first {
            if line.starts_with(b"\xEF\xBB\xBF") {
                return Err(refused());
            }
            first = false;
        }
        let text = line.strip_suffix(b"\n").unwrap_or(&line);
        let text = text.strip_suffix(b"\r").unwrap_or(text);
        match source_id_value(text) {
            None => {}
            Some(Some(value)) if value == SOURCE_ID.as_bytes() => return Ok(()),
            Some(_) => return Err(refused()),
        }
    }
}

/// `None` when the line is not a `#define SQLITE_SOURCE_ID` directive; `Some(None)` when it is one but its
/// value is not one double-quoted byte string followed only by horizontal whitespace.
fn source_id_value(line: &[u8]) -> Option<Option<&[u8]>> {
    let rest = line.strip_prefix(b"#define")?;
    let rest = skip_blanks(rest)?;
    let rest = rest.strip_prefix(b"SQLITE_SOURCE_ID")?;
    let rest = skip_blanks(rest)?;
    let Some(quoted) = rest.strip_prefix(b"\"") else {
        return Some(None);
    };
    let Some(end) = quoted.iter().position(|&byte| byte == b'"') else {
        return Some(None);
    };
    let Some((value, closing)) = quoted.split_at_checked(end) else {
        return Some(None);
    };
    let Some(after) = closing.get(1..) else {
        return Some(None);
    };
    if after.iter().all(|&byte| byte == b' ' || byte == b'\t') {
        Some(Some(value))
    } else {
        Some(None)
    }
}

/// The bytes after at least one leading space or tab; `None` when there is none.
fn skip_blanks(bytes: &[u8]) -> Option<&[u8]> {
    let count = bytes
        .iter()
        .take_while(|&&byte| byte == b' ' || byte == b'\t')
        .count();
    if count == 0 { None } else { bytes.get(count..) }
}

/// SHA-256 of Cargo.lock and every regular file under src/, hashed in-process.
fn source_hash(manifest_dir: &Path) -> Result<String, BuildFailure> {
    let lock = manifest_dir.join("Cargo.lock");
    let lock_meta =
        fs::metadata(&lock).map_err(|error| BuildFailure::io("hash", "metadata", &lock, error))?;
    if !lock_meta.is_file() {
        return Err(BuildFailure::refusal(
            "hash",
            "not a regular file or directory",
        ));
    }
    let src = manifest_dir.join("src");
    let src_kind = fs::symlink_metadata(&src)
        .map_err(|error| BuildFailure::io("hash", "metadata", &src, error))?
        .file_type();
    if src_kind.is_symlink() {
        return Err(BuildFailure::refusal("hash", "symbolic link in src"));
    }
    if !src_kind.is_dir() {
        return Err(BuildFailure::refusal(
            "hash",
            "not a regular file or directory",
        ));
    }
    let mut sources = Vec::new();
    collect_sources(&src, b"src", &mut sources)?;
    sources.sort_unstable_by(|left, right| left.0.cmp(&right.0));
    let mut records = Vec::with_capacity(sources.len() + 1);
    records.push((b"Cargo.lock".to_vec(), lock));
    records.extend(sources);

    let mut hasher = Sha256::new();
    hasher.update(HASH_PREFIX);
    let mut buffer = [0_u8; HASH_BUFFER_BYTES];
    for (name, path) in &records {
        hash_record(&mut hasher, &mut buffer, name, path)?;
    }
    Ok(format!("{:x}", hasher.finalize()))
}

/// Appends every regular file below `dir` with its root-relative name; refuses links and special files.
fn collect_sources(
    dir: &Path,
    name: &[u8],
    sources: &mut Vec<(Vec<u8>, PathBuf)>,
) -> Result<(), BuildFailure> {
    let entries =
        fs::read_dir(dir).map_err(|error| BuildFailure::io("hash", "read", dir, error))?;
    for entry in entries {
        let entry = entry.map_err(|error| BuildFailure::io("hash", "read", dir, error))?;
        let path = entry.path();
        let kind = entry
            .file_type()
            .map_err(|error| BuildFailure::io("hash", "metadata", &path, error))?;
        let mut child_name = Vec::with_capacity(name.len() + 1 + entry.file_name().len());
        child_name.extend_from_slice(name);
        child_name.push(b'/');
        child_name.extend_from_slice(entry.file_name().as_bytes());
        if kind.is_symlink() {
            return Err(BuildFailure::refusal("hash", "symbolic link in src"));
        } else if kind.is_dir() {
            collect_sources(&path, &child_name, sources)?;
        } else if kind.is_file() {
            sources.push((child_name, path));
        } else {
            return Err(BuildFailure::refusal(
                "hash",
                "not a regular file or directory",
            ));
        }
    }
    Ok(())
}

/// Hashes one record: its name, NUL, decimal length, NUL, exactly that many file bytes, NUL. The length is
/// the opened file's, and the bytes streamed through `buffer` must match it exactly.
fn hash_record(
    hasher: &mut Sha256,
    buffer: &mut [u8; HASH_BUFFER_BYTES],
    name: &[u8],
    path: &Path,
) -> Result<(), BuildFailure> {
    let file = File::open(path).map_err(|error| BuildFailure::io("hash", "open", path, error))?;
    let length = file
        .metadata()
        .map_err(|error| BuildFailure::io("hash", "metadata", path, error))?
        .len();
    hasher.update(name);
    hasher.update(b"\0");
    hasher.update(length.to_string().as_bytes());
    hasher.update(b"\0");
    let mut reader = file.take(length);
    let mut copied: u64 = 0;
    loop {
        let count = match reader.read(buffer) {
            Ok(0) => break,
            Ok(count) => count,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(BuildFailure::io("hash", "read", path, error)),
        };
        let (chunk, _) = buffer
            .split_at_checked(count)
            .expect("Read::read returns at most the buffer's length");
        hasher.update(chunk);
        let count =
            u64::try_from(count).expect("usize is 64 bits on the x86_64 host read_config requires");
        // Cannot overflow: the reader is file.take(length), so copied never exceeds length.
        copied = copied.strict_add(count);
    }
    if copied != length {
        return Err(BuildFailure::refusal(
            "hash",
            "file length changed while hashing",
        ));
    }
    hasher.update(b"\0");
    Ok(())
}

/// Compiles src/sqlite_unit.c alone with the C compile options for the mode read_config chose, into
/// OUT_DIR/sqlite_unit.o. A profile-use build writes its object at the profile directory's sqlite_unit.o,
/// the path build.sh's instrumented compile used, because GCC names the profile after the object's whole
/// path and -fprofile-prefix-path strips nothing on GCC 13.
fn compile_unit(config: &Config) -> Result<PathBuf, BuildFailure> {
    let object = match &config.pgo_dir {
        Some(dir) => Path::new(dir).join("sqlite_unit.o"),
        None => Path::new(&config.out_dir).join("sqlite_unit.o"),
    };
    let sanitized = !config.sanitizers.is_empty();
    let mut command = Command::new(&config.compiler);
    command
        .env_clear()
        .env("PATH", CHILD_PATH)
        .arg(if sanitized { "-O1" } else { "-O3" })
        .arg("-g")
        .arg("-fno-semantic-interposition")
        .arg(format!("-march={}", config.c_target))
        .args(WARNING_FLAGS)
        .arg(format!("-Wframe-larger-than={STACK_BUFFER_MAX_BYTES}"))
        .args(GCC_WARNING_FLAGS)
        .arg("-isystem")
        .arg(config.manifest_dir.join("vendor"))
        .args(SQLITE_DEFINES);
    if config.fault_inject {
        command.arg("-DTWELVE_FAULT_INJECT=1");
    }
    if config.safe {
        command.arg("-DTWELVE_SAFE_BUILD=1");
        // UNSAFE PATH (speed): C frame pointers omitted; the fast build compiles the C unit without -fno-omit-frame-pointer, which takes a register from every function of the unity unit, SQLite included, and -g still maps each address to its source line.
        command.arg("-fno-omit-frame-pointer");
        if !sanitized {
            // UNSAFE PATH (speed): no C hardening flags; the fast build compiles the C unit without the release hardening flags, which change the generated code of SQLite's hot paths, and readers.c and checkpointer.c are written to be correct without them.
            command.args(HARDENING_FLAGS);
        }
    }
    if sanitized {
        for kind in &config.sanitizers {
            command.arg(format!("-fsanitize={kind}"));
        }
        command
            .arg("-fno-sanitize-recover=all")
            .arg("-fno-omit-frame-pointer");
    }
    if let Some(dir) = &config.pgo_dir {
        command
            .arg(format!("-fprofile-use={dir}"))
            .arg("-fprofile-correction")
            .arg("-fprofile-partial-training")
            .arg("-Werror=missing-profile");
    }
    command
        .arg("-c")
        .arg(config.manifest_dir.join("src/sqlite_unit.c"))
        .arg("-o")
        .arg(&object);
    run_child(&mut command, &config.compiler, "compile")?;
    Ok(object)
}

/// Replaces OUT_DIR/libsqlite3.a with an archive of the one unit object.
fn archive_unit(config: &Config, object: &Path) -> Result<(), BuildFailure> {
    let library = Path::new(&config.out_dir).join("libsqlite3.a");
    match fs::remove_file(&library) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(BuildFailure::io("archive", "remove", &library, error)),
    }
    let archiver = Path::new(ARCHIVER);
    let mut command = Command::new(archiver);
    command
        .env_clear()
        .env("PATH", CHILD_PATH)
        .arg("crs")
        .arg(&library)
        .arg(object);
    run_child(&mut command, archiver, "archive")
}

/// Runs a child to its end: a spawn failure names the program, and a failed status is an exit code, or a
/// signal death with its core flag, never a made-up code.
fn run_child(
    command: &mut Command,
    program: &Path,
    stage: &'static str,
) -> Result<(), BuildFailure> {
    let status = command
        .status()
        .map_err(|error| BuildFailure::io(stage, "spawn", program, error))?;
    if status.success() {
        return Ok(());
    }
    let detail = match (status.code(), status.signal()) {
        (Some(code), _) => format!("child exited {code}"),
        (None, Some(signal)) if status.core_dumped() => {
            format!("child killed by signal {signal} (core dumped)")
        }
        (None, Some(signal)) => format!("child killed by signal {signal}"),
        // A wait status without WUNTRACED is an exit or a signal death; std's own text names any other.
        (None, None) => status.to_string(),
    };
    Err(BuildFailure::refusal(stage, detail))
}

fn rerun_directives(config: &Config) -> String {
    let mut text = String::new();
    for path in RERUN_PATHS {
        text.push_str("cargo::rerun-if-changed=");
        text.push_str(path);
        text.push('\n');
    }
    if let Some(dir) = &config.pgo_dir {
        text.push_str("cargo::rerun-if-changed=");
        text.push_str(dir);
        text.push('\n');
    }
    for name in RERUN_VARS {
        text.push_str("cargo::rerun-if-env-changed=");
        text.push_str(name);
        text.push('\n');
    }
    text
}

/// The five compile-time facts for main.rs, then the link directives and the cfg declarations.
fn build_facts(config: &Config, digest: &str) -> String {
    let pgo = if config.pgo_dir.is_some() { "1" } else { "0" };
    let mut text = format!(
        "cargo::rustc-env=TWELVE_SOURCE_SHA256={digest}\n\
         cargo::rustc-env=TWELVE_RUSTFLAGS={}\n\
         cargo::rustc-env=TWELVE_C_TARGET={}\n\
         cargo::rustc-env=TWELVE_PGO={pgo}\n\
         cargo::rustc-env=TWELVE_SQLITE_SOURCE_ID={SOURCE_ID}\n\
         cargo::rustc-link-search=native={}\n\
         cargo::rustc-link-lib=static=sqlite3\n\
         cargo::rustc-link-lib=m\n",
        json_array(&config.rust_flags),
        config.c_target,
        config.out_dir,
    );
    for name in CFG_FLAGS {
        text.push_str("cargo::rustc-check-cfg=cfg(");
        text.push_str(name);
        text.push_str(")\n");
    }
    text
}

/// Compact JSON array of strings: `"` and `\` backslash-escaped, U+0000 to U+001F as `\u00xx`, every
/// other character unchanged.
fn json_array(items: &[String]) -> String {
    let mut text = String::from("[");
    for (index, item) in items.iter().enumerate() {
        if index > 0 {
            text.push(',');
        }
        text.push('"');
        for character in item.chars() {
            match character {
                '"' => text.push_str("\\\""),
                '\\' => text.push_str("\\\\"),
                control if u32::from(control) < 0x20 => {
                    text.push_str(&format!("\\u{:04x}", u32::from(control)));
                }
                other => text.push(other),
            }
        }
        text.push('"');
    }
    text.push(']');
    text
}

/// Writes Cargo directives; a failed write is a metadata failure, never a silently truncated stream.
fn emit(text: &str) -> Result<(), BuildFailure> {
    let mut out = io::stdout().lock();
    out.write_all(text.as_bytes())
        .and_then(|()| out.flush())
        .map_err(|error| BuildFailure {
            stage: "metadata",
            detail: String::from("write stdout"),
            source: Some(error),
        })
}
