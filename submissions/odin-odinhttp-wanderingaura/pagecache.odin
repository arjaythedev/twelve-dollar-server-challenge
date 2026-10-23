package main

import "core:fmt"
import "core:strings"
import "core:sys/linux"
import "core:time"

// Installing and building leaves ~1 GB of package, toolchain and build files in the page cache.
// On a 2 GB box that pushes the kernel into reclaim at high connection counts (p99 went from
// 24 ms to 1.4 s at 100k connections in a 2 GB container). So at startup, before the database is
// opened, the server asks the kernel to drop the cached pages of every file it can read under
// /usr, /var, /etc and its own directory: posix_fadvise(DONTNEED), file by file. No privileges
// or kernel settings involved; only clean, unmapped pages are dropped. The database and its WAL
// are skipped so their pages stay cached.

POSIX_FADV_DONTNEED :: 4

File_ID :: struct {
	dev: linux.Dev,
	ino: linux.Inode,
}

drop_page_cache :: proc(sqlite_path: string) {
	start := time.tick_now()
	linux.sync() // dirty pages can't be dropped until written back

	keep: [dynamic]File_ID
	defer delete(keep)
	for suffix in ([]string{"", "-wal", "-shm"}) {
		st: linux.Stat
		path := strings.clone_to_cstring(strings.concatenate({sqlite_path, suffix}, context.temp_allocator), context.temp_allocator)
		if linux.stat(path, &st) == nil {
			append(&keep, File_ID{st.dev, st.ino})
		}
	}

	roots := [4]string{"/usr", "/var", "/etc", ""}
	exe: [4096]u8
	if n, err := linux.readlink("/proc/self/exe", exe[:]); err == nil && n < len(exe) {
		// The submission directory: the binary is <submission>/bin/server.
		path := string(exe[:n])
		for _ in 0 ..< 2 {
			if i := strings.last_index_byte(path, '/'); i > 0 { path = path[:i] }
		}
		roots[3] = path
	}

	files := 0
	for root in roots {
		if root == "" { continue }
		fd, err := linux.open(strings.clone_to_cstring(root, context.temp_allocator), {.DIRECTORY, .NOFOLLOW, .CLOEXEC})
		if err == nil {
			files += drop_dir(fd, keep[:], 0)
		}
	}
	fmt.printfln("dropped the page cache of %d files in %v", files, time.tick_since(start))
}

// Closes `dir`. Each level reads entries into its own stack buffer, since it recurses mid-listing.
@(private = "file")
drop_dir :: proc(dir: linux.Fd, keep: []File_ID, depth: int) -> (files: int) {
	defer linux.close(dir)
	if depth > 64 { return }

	buf: [16 * 1024]u8
	for {
		n, err := linux.getdents(dir, buf[:])
		if err != nil || n <= 0 { return }
		offset := 0
		for d in linux.dirent_iterate_buf(buf[:n], &offset) {
			name := linux.dirent_name(d)
			if name == "." || name == ".." { continue }
			cname := cstring(([^]u8)(&d.name))

			#partial switch d.type {
			case .DIR:
				sub, sub_err := linux.openat(dir, cname, {.DIRECTORY, .NOFOLLOW, .CLOEXEC})
				if sub_err == nil {
					files += drop_dir(sub, keep, depth + 1)
				}
			case .REG:
				f, open_err := linux.openat(dir, cname, {.NOFOLLOW, .NONBLOCK, .CLOEXEC})
				if open_err != nil { continue }
				st: linux.Stat
				skip := false
				if linux.fstat(f, &st) == nil {
					for k in keep {
						if k.dev == st.dev && k.ino == st.ino { skip = true }
					}
				}
				if !skip {
					linux.syscall(linux.SYS_fadvise64, f, i64(0), i64(0), i32(POSIX_FADV_DONTNEED))
					files += 1
				}
				linux.close(f)
			}
			// Symlinks are not followed; other types have no page cache to drop.
		}
	}
}
