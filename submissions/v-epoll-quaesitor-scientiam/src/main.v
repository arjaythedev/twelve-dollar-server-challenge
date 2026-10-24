module main

import os

fn main() {
	if os.args.len == 3 && os.args[1] == '--train' {
		$if linux {
			run_training(os.args[2]) or {
				eprintln('fatal: ${err}')
				exit(1)
			}
			exit(0)
		} $else {
			eprintln('--train is only available on linux')
			exit(1)
		}
	}
	host := os.getenv_opt('HOST') or { '0.0.0.0' }
	port := (os.getenv_opt('PORT') or { '3000' }).int()
	if port <= 0 {
		eprintln('fatal: bad PORT')
		exit(1)
	}
	$if linux && !use_fasthttp ? {
		run_epoll(host, port) or {
			eprintln('fatal: ${err}')
			exit(1)
		}
	} $else {
		run_fasthttp(host, port) or {
			eprintln('fatal: ${err}')
			exit(1)
		}
	}
}
