package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"
	"syscall"
	"time"

	"feed/internal/config"
	"feed/internal/httpapi"
	"feed/internal/repository"
	"feed/internal/service"
)

// tuned for the 1 vCPU, 2 GB RAM droplet
const (
	// GC runs only near this limit, leaving the rest of RAM to the OS and
	// SQLite's page cache and mmap
	memLimit = 768 << 20
)

func main() {
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(-1)
	debug.SetMemoryLimit(memLimit)

	if err := run(); err != nil {
		slog.Error("server", slog.Any("err", err))
		os.Exit(1)
	}
}

func run() error {
	cfg := config.Parse()

	db, err := repository.Open(cfg.SQLitePath)
	if err != nil {
		return fmt.Errorf("open db: %w", err)
	}
	defer db.Close()

	srv := &http.Server{
		Addr:              cfg.Addr,
		Handler:           httpapi.New(db, service.New(repository.New(db)), cfg.JWTSecret),
		ReadHeaderTimeout: 10 * time.Second,
		// longer than Nginx's 65 s keepalive, so it never reuses a socket
		// we just closed
		IdleTimeout: 120 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx) //nolint:contextcheck
	}()

	slog.Info("listening", slog.String("addr", cfg.Addr))
	if err := srv.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
		return fmt.Errorf("serve: %w", err)
	}
	return nil
}
