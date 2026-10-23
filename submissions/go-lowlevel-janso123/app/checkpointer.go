package main

import (
	"database/sql"
	"io"
	"os"
	"runtime"
	"sync/atomic"
	"time"
)

// The buffered notification coalesces requests; no writer operation waits for
// SQLite maintenance. All SQLite calls on conn happen on this locked thread.
type checkpointer struct {
	path                      string
	signalCh                  chan struct{}
	stop                      chan struct{}
	done                      chan struct{}
	stats                     *writerMetrics
	completeSize              atomic.Uint64
	previousLog, previousDone int
}

const checkpointRestartBytes = 16 << 20

func startCheckpointer(path string, diagnostics io.Writer, stats *writerMetrics) (*checkpointer, error) {
	c := &checkpointer{path: path, signalCh: make(chan struct{}, 1), stop: make(chan struct{}), done: make(chan struct{}), stats: stats}
	ready := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		defer close(c.done)
		db, conn, err := openSQLite(path, diagnostics)
		if err != nil {
			ready <- err
			return
		}
		defer db.Close()
		defer conn.Close()
		if _, err = conn.ExecContext(ctx, "PRAGMA wal_autocheckpoint=0"); err != nil {
			ready <- err
			return
		}
		var pageSize int
		if err = conn.QueryRowContext(ctx, "PRAGMA page_size").Scan(&pageSize); err != nil {
			ready <- err
			return
		}
		ready <- nil
		var last time.Time
		for {
			select {
			case <-c.stop:
				// The loop and writer have stopped, so shrinking on close is safe.
				c.run(conn, "TRUNCATE")
				return
			case <-c.signalCh:
				size := walFileSize(path)
				if size >= 0 {
					maximum(&stats.walMax, uint64(size))
				}
				if time.Since(last) < time.Second {
					continue
				}
				complete, logFrames := c.run(conn, "PASSIVE")
				if complete && int64(logFrames)*int64(pageSize) >= checkpointRestartBytes {
					// PASSIVE already did the potentially slow DB sync. A zero-timeout
					// RESTART now only tries to secure a reader-free reset point.
					c.run(conn, "RESTART")
				}
				last = time.Now()
			}
		}
	}()
	if err := <-ready; err != nil {
		<-c.done
		return nil, err
	}
	return c, nil
}

func (c *checkpointer) signal() {
	select {
	case c.signalCh <- struct{}{}:
	default:
	}
}

func (c *checkpointer) signalIfNeeded(size int64) {
	if size >= 0 {
		maximum(&c.stats.walMax, uint64(size))
	}
	if size < 0 || size >= checkpointSoftBytes {
		c.signal()
	}
}

func (c *checkpointer) shutdown() { close(c.stop); <-c.done }

func walFileSize(path string) int64 {
	if path == "" {
		return 0
	}
	stat, err := os.Stat(path + "-wal")
	if os.IsNotExist(err) {
		return 0
	}
	if err != nil {
		return -1
	}
	return stat.Size()
}

func (c *checkpointer) run(conn *sql.Conn, mode string) (bool, int) {
	start := time.Now()
	var busy, logFrames, doneFrames int
	err := conn.QueryRowContext(ctx, "PRAGMA wal_checkpoint("+mode+")").Scan(&busy, &logFrames, &doneFrames)
	duration := uint64(time.Since(start))
	c.stats.checkpoints.Add(1)
	c.stats.checkpointNS.Add(duration)
	maximum(&c.stats.checkpointMax, duration)
	if mode == "RESTART" {
		c.stats.restartCount.Add(1)
		maximum(&c.stats.restartMax, duration)
		if err != nil || busy != 0 {
			c.stats.restartBusy.Add(1)
		}
	}
	if logFrames >= 0 && doneFrames >= 0 {
		if logFrames < c.previousLog {
			c.stats.walRestarts.Add(1)
			c.previousDone = 0
		}
		newFrames := doneFrames - c.previousDone
		if newFrames < 0 {
			newFrames = doneFrames
		}
		c.stats.checkpointFrames.Add(uint64(newFrames))
		c.stats.checkpointLogFrames.Store(uint64(logFrames))
		c.stats.checkpointDoneFrames.Store(uint64(doneFrames))
		c.previousLog, c.previousDone = logFrames, doneFrames
	}
	complete := err == nil && busy == 0 && logFrames >= 0 && doneFrames == logFrames
	if !complete {
		c.stats.checkpointBusy.Add(1)
	} else if mode == "PASSIVE" || mode == "RESTART" {
		if size := walFileSize(c.path); size >= 0 {
			c.completeSize.Store(uint64(size))
		}
	}
	return complete, logFrames
}
