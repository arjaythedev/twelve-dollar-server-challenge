//go:build linux && !amd64 && !arm64

package main

import (
	"fmt"
	"os"
)

func loop(listener int, a *app, w *writer, stop <-chan os.Signal, onStop func() error) error {
	fmt.Fprintln(os.Stderr, "network_backend=epoll fallback_reason=unsupported_architecture")
	return epollLoop(listener, a, w, stop, onStop)
}
