//go:build linux && (amd64 || arm64)

package main

import (
	"fmt"
	"golang.org/x/sys/unix"
	"os"
)

func directTableSize() (int, error) {
	var limit unix.Rlimit
	if e := unix.Getrlimit(unix.RLIMIT_NOFILE, &limit); e != nil {
		return 0, e
	}
	if limit.Cur < limit.Max {
		limit.Cur = limit.Max
		if e := unix.Setrlimit(unix.RLIMIT_NOFILE, &limit); e != nil {
			return 0, fmt.Errorf("raise RLIMIT_NOFILE: %w", e)
		}
	}
	if limit.Cur == 0 || limit.Cur > 1<<28-1 {
		return 0, fmt.Errorf("RLIMIT_NOFILE %d exceeds direct slot ID range", limit.Cur)
	}
	return int(limit.Cur), nil
}

func loop(listener int, a *app, w *writer, stop <-chan os.Signal, onStop func() error) error {
	mode := env("NETWORK_BACKEND", "auto")
	var fallback error
	switch mode {
	case "epoll":
	case "auto", "uring-direct":
		slots, e := directTableSize()
		if e == nil {
			b, openErr := newDirectBackend(slots)
			if openErr == nil {
				fmt.Fprintf(os.Stderr, "network_backend=uring-direct requested=%s\n", mode)
				return b.loop(listener, a, w, stop, onStop)
			}
			e = openErr
		}
		fallback = fmt.Errorf("uring-direct: %w", e)
		b, normalErr := newUringBackend()
		if normalErr == nil {
			fmt.Fprintf(os.Stderr, "network_backend=uring requested=%s fallback_reason=%q\n", mode, fallback)
			return b.loop(listener, a, w, stop, onStop)
		}
		fallback = fmt.Errorf("%v; uring: %w", fallback, normalErr)
	case "uring":
		b, e := newUringBackend()
		if e == nil {
			fmt.Fprintf(os.Stderr, "network_backend=uring requested=%s\n", mode)
			return b.loop(listener, a, w, stop, onStop)
		}
		fallback = fmt.Errorf("uring: %w", e)
	default:
		fallback = fmt.Errorf("unknown NETWORK_BACKEND %q (expected auto, epoll, uring or uring-direct)", mode)
	}
	if fallback == nil {
		fmt.Fprintf(os.Stderr, "network_backend=epoll requested=%s\n", mode)
	} else {
		fmt.Fprintf(os.Stderr, "network_backend=epoll requested=%s fallback_reason=%q\n", mode, fallback)
	}
	return epollLoop(listener, a, w, stop, onStop)
}
