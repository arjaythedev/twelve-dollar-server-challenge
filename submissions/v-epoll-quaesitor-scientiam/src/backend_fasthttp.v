module main

import fasthttp
import os

// The portable backend: vlib's fasthttp reactor (epoll on Linux, kqueue on macOS/BSD,
// IOCP on Windows). One App per worker thread via make_state; the reactor offers no
// end-of-batch hook, so each request that wrote commits before its response is sent.

fn fh_make_state() voidptr {
	// fasthttp starts one worker per CPU, each with its own connection: no exclusive lock
	app := new_app(true, false) or {
		eprintln('fatal: ${err}')
		exit(1)
	}
	return app
}

fn fh_handle(req fasthttp.HttpRequest, mut out []u8, worker_state voidptr, mut ctl fasthttp.ResponseControl) fasthttp.Step {
	mut app := unsafe { &App(worker_state) }
	app.tick()
	start := out.len
	oc := app.handle(req, mut out)
	if !app.batch_end() && oc.wrote {
		// the write was rolled back: the tentative 201 must not leave the process
		unsafe {
			out.len = start
		}
		put_error(mut out, status_500, 'internal server error', false)
	}
	if oc.close {
		ctl.should_close = true
		return .close
	}
	return .done
}

// run_fasthttp serves HOST:PORT on the fasthttp reactor until the process is stopped.
fn run_fasthttp(host string, port int) ! {
	mut server := fasthttp.new_server(fasthttp.ServerConfig{
		family:         .ip
		host:           host
		port:           port
		append_handler: fh_handle
		make_state:     fh_make_state
	})!
	println('listening on http://${host}:${port}/ (fasthttp backend, pid ${os.getpid()})')
	flush_stdout()
	server.run()!
}
