package main

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"
	"syscall"
	"time"

	"github.com/lesismal/fib"
	fibhttp "github.com/lesismal/fib/http"

	"feed/internal/config"
	"feed/internal/httpapi"
	"feed/internal/repository"
	"feed/internal/service"
)

// tuned for the 1 vCPU, 2 GB RAM droplet
const (
	// the live heap is small: GC stays cheap, and reused spans beat the fresh
	// zeroed memory GC off takes for every allocation
	gcPercent = 400
	// the heap never grows past this, leaving the rest of RAM to the OS and
	// SQLite's page cache and mmap
	memLimit = 768 << 20
)

func main() {
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(gcPercent)
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

	srv := httpapi.New(db, service.New(repository.New(db)), cfg.JWTSecret)
	// fib's event loops hold idle connections without a goroutine or buffers.
	// Its defaults time out a header after 10 s and an idle connection after 2 min.
	engine, err := fib.New(fib.DefaultConfig(), fib.Binding{
		Addr:    cfg.Addr,
		Handler: fibhttp.NewHandler(srv),
	})
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	defer engine.Close()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		// in-flight requests get to finish; idle connections close at once
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = engine.Shutdown(shutdownCtx) //nolint:contextcheck
	}()

	slog.Info("listening", slog.String("addr", cfg.Addr))
	if err := engine.Run(); err != nil {
		return fmt.Errorf("serve: %w", err)
	}
	return nil
}
