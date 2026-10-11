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
	hc := fibhttp.DefaultConfig()
	hc.ReadHeaderTimeout = 10 * time.Second
	hc.IdleTimeout = 120 * time.Second
	ec := fib.DefaultConfig()
	ec.Addr = cfg.Addr
	// fib's event loops hold idle connections without a goroutine or buffers
	engine, err := fib.Bind(ec, fibhttp.NewHandlerWithConfig(hc, srv))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	defer engine.Close()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		engine.Stop()
	}()

	slog.Info("listening", slog.String("addr", cfg.Addr))
	if err := engine.Run(); err != nil {
		return fmt.Errorf("serve: %w", err)
	}
	return nil
}
