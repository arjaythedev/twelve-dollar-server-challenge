package main

import "core:fmt"
import "core:nbio"
import "core:net"
import "core:os"
import "core:strconv"
import "core:time"

import http "http:."

env :: proc(key: string, fallback := "") -> string {
	v, found := os.lookup_env(key, context.allocator)
	return v if found && v != "" else fallback
}

main :: proc() {
	started = time.tick_now()

	sqlite_path := env("SQLITE_PATH")
	if sqlite_path == "" {
		fmt.eprintln("SQLITE_PATH is required")
		os.exit(1)
	}
	host := env("HOST", "0.0.0.0")
	port, port_ok := strconv.parse_int(env("PORT", "3000"), 10)
	address := net.parse_address(host)
	if !port_ok || address == nil {
		fmt.eprintfln("bad HOST/PORT: %q %v", host, port)
		os.exit(1)
	}

	raise_nofile_limit()

	// Memory first, all of it touched now.
	out.buf = raw_data(slab_alloc(OUT_BUF_SIZE))
	when PGO_TRAIN {
		pgo_train(sqlite_path)
		return
	}
	http.pool_init(slab_alloc(REQUEST_BLOCKS * http.POOL_BLOCK_SIZE))
	conns: Conn_Allocator
	conn_allocator := conn_allocator_init(&conns)

	jwt_init(env("JWT_SECRET", "twelve-dollar-challenge"))
	drop_page_cache(sqlite_path)
	db_open(sqlite_path)
	db_warm_up()

	// http.listen, but with one thread (the droplet has one vCPU and SQLite is built
	// single-threaded), the slab allocator and a full accept backlog.
	s: http.Server
	s.opts = http.Default_Server_Opts
	s.opts.thread_count = 1
	s.conn_allocator = conn_allocator
	if err := nbio.acquire_thread_event_loop(); err != nil {
		fmt.eprintfln("event loop: %v", err)
		os.exit(1)
	}
	listen_err: net.Network_Error
	s.tcp_sock, listen_err = nbio.listen_tcp({address, port}, 65535)
	if listen_err != nil {
		fmt.eprintfln("listen on %s:%d: %v", host, port, listen_err)
		os.exit(1)
	}
	fmt.printfln("listening on %s:%d (startup %v)", host, port, time.tick_since(started))

	if err := http.serve(&s, http.handler(handle)); err != nil {
		fmt.eprintfln("serve: %v", err)
		os.exit(1)
	}
}
