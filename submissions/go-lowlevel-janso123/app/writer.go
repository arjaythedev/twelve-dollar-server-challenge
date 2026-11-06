package main

import (
	"database/sql"
	"errors"
	"fmt"
	"io"
	"log"
	"runtime"
	"sync"
	"time"

	sqlite3 "github.com/mattn/go-sqlite3"
	"golang.org/x/sys/unix"
)

const defaultWriteQueue = 4096
const checkpointSoftBytes = 4 << 20
const checkpointHardBytes = 256 << 20
const maxBeginWait = 10 * time.Second

type writeRequest struct {
	fd           int32
	generation   uint32
	user, post   int64
	body, author string
	create       bool
	enqueued     time.Time
}
type writeCompletion struct {
	fd         int32
	generation uint32
	reply      reply
}
type writer struct {
	conn             *sql.Conn
	statements       map[string]*sql.Stmt
	a                *app
	path             string
	queue            chan writeRequest
	results          chan writeCompletion
	slots            chan struct{} // includes queued, in-flight and unconsumed completions
	done             chan struct{}
	wake             int
	stats            *writerMetrics
	once             sync.Once
	checkpointer     *checkpointer
	diagnostics      io.Writer
	lastErrorLog     time.Time
	suppressedErrors uint64
}

func startWriter(path string, secret []byte, capacity int, diagnostics io.Writer, stats *writerMetrics) (*writer, error) {
	wake, e := unix.Eventfd(0, unix.EFD_NONBLOCK|unix.EFD_CLOEXEC)
	if e != nil {
		return nil, e
	}
	w := &writer{path: path, wake: wake, queue: make(chan writeRequest, capacity), results: make(chan writeCompletion, capacity), slots: make(chan struct{}, capacity), done: make(chan struct{}), stats: stats, diagnostics: diagnostics}
	ready := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		defer close(w.done)
		db, c, e := openSQLite(path, diagnostics)
		if e != nil {
			ready <- e
			return
		}
		defer db.Close()
		defer c.Close()
		w.conn = c
		if _, e = c.ExecContext(ctx, "PRAGMA wal_autocheckpoint=0"); e != nil {
			ready <- e
			return
		}
		if _, e = c.ExecContext(ctx, "PRAGMA busy_timeout=100"); e != nil {
			ready <- e
			return
		}
		a, e := newApp(c, secret, path)
		if e != nil {
			ready <- e
			return
		}
		defer a.close()
		// Health never shares the writer handle. SELECT 1 stays exactly the same.
		healthDB, health, e := openSQLite(path, diagnostics)
		if e != nil {
			ready <- e
			return
		}
		defer healthDB.Close()
		defer health.Close()
		a.statements["health"].Close()
		a.statements["health"], e = health.PrepareContext(ctx, "SELECT 1")
		if e != nil {
			ready <- e
			return
		}
		w.a = a
		w.statements = a.statements
		w.checkpointer, e = startCheckpointer(path, diagnostics, stats)
		if e != nil {
			ready <- e
			return
		}
		ready <- nil
		w.work()
		a.reader.close()          // loop has stopped; release the final read snapshot
		w.checkpointer.shutdown() // final maintenance stays on its own thread
	}()
	if e = <-ready; e != nil {
		<-w.done
		unix.Close(wake)
		return nil, e
	}
	return w, nil
}
func (w *writer) submit(q writeRequest) bool {
	select {
	case w.slots <- struct{}{}:
	default:
		w.stats.rejected.Add(1)
		return false
	}
	q.enqueued = time.Now()
	w.queue <- q // a slot guarantees room, and only the loop submits
	maximum(&w.stats.queueMax, uint64(len(w.slots)))
	return true
}
func (w *writer) shutdown() { w.once.Do(func() { close(w.queue); <-w.done }) }
func (w *writer) work() {
	requests := make([]writeRequest, cap(w.queue))
	results := make([]writeCompletion, cap(w.queue))
	for {
		first, ok := <-w.queue
		if !ok {
			return
		}
		requests[0] = first
		n := 1
	drain:
		for n < len(requests) {
			select {
			case q, ok := <-w.queue:
				if !ok {
					break drain
				}
				requests[n] = q
				n++
			default:
				break drain
			}
		}
		start := time.Now()
		for i := 0; i < n; i++ {
			if !requests[i].enqueued.IsZero() {
				w.stats.queueWait(start.Sub(requests[i].enqueued))
			}
		}
		w.batch(requests[:n], results[:n])
		// Publish committed responses before signalling maintenance.
		for i := 0; i < n; i++ {
			w.results <- results[i]
			requests[i] = writeRequest{}
			results[i] = writeCompletion{}
		}
		var wake = [8]byte{1}
		for {
			_, e := unix.Write(w.wake, wake[:])
			if e == unix.EINTR {
				continue
			}
			if e != nil && e != unix.EAGAIN {
				log.Fatalf("writer eventfd: %v", e)
			}
			break
		}
		w.checkpointer.signalIfNeeded(w.walSize())
	}
}
func (w *writer) walSize() int64 {
	return walFileSize(w.path)
}
func (w *writer) logWriteFailure(phase, operation string, err error) {
	w.stats.writeErrors.Add(1)
	switch phase {
	case "BEGIN":
		w.stats.writeErrorsBegin.Add(1)
	case "COMMIT":
		w.stats.writeErrorsCommit.Add(1)
	default:
		w.stats.writeErrorsStatement.Add(1)
	}
	var sqlErr sqlite3.Error
	code, extended := 0, 0
	if errors.As(err, &sqlErr) {
		code, extended = int(sqlErr.Code), int(sqlErr.ExtendedCode)
		if sqlErr.Code == sqlite3.ErrBusy {
			w.stats.writeErrorsBusy.Add(1)
		}
	}
	if w.diagnostics == nil {
		return
	}
	now := time.Now()
	if now.Sub(w.lastErrorLog) < time.Second {
		w.suppressedErrors++
		return
	}
	fmt.Fprintf(w.diagnostics, "write_failure phase=%s operation=%s sqlite_code=%d sqlite_extended=%d error=%q suppressed_since_last=%d\n", phase, operation, code, extended, err, w.suppressedErrors)
	w.lastErrorLog, w.suppressedErrors = now, 0
}
func (w *writer) beginWrite() error {
	deadline := time.Now().Add(maxBeginWait)
	for {
		// Acquire WAL_WRITE_LOCK before the first savepoint or statement. If a
		// RESTART owns it, SQLITE_BUSY here has no write side effects to replay.
		_, err := w.conn.ExecContext(ctx, "BEGIN IMMEDIATE")
		if err == nil {
			return nil
		}
		var sqlErr sqlite3.Error
		if !errors.As(err, &sqlErr) || sqlErr.Code != sqlite3.ErrBusy {
			w.logWriteFailure("BEGIN", "begin_immediate", err)
			return err
		}
		if !time.Now().Before(deadline) {
			w.stats.beginBusyTimeouts.Add(1)
			w.logWriteFailure("BEGIN", "begin_immediate", err)
			return err
		}
		w.stats.beginBusyRetries.Add(1)
		time.Sleep(2 * time.Millisecond)
	}
}
func (w *writer) batch(qs []writeRequest, rs []writeCompletion) {
	internal := failure(500, "internal server error")
	for i, q := range qs {
		rs[i] = writeCompletion{fd: q.fd, generation: q.generation, reply: internal}
	}
	// Physical WAL length is a high-water mark: a completed PASSIVE checkpoint
	// permits frame reuse without truncating the file. Stop only if it grows
	// beyond the last fully checkpointed high-water mark.
	size := w.walSize()
	if size >= 0 {
		maximum(&w.stats.walMax, uint64(size))
	}
	if size < 0 {
		for i := range rs {
			rs[i].reply = failure(503, "writer unavailable")
		}
		w.stats.rejected.Add(uint64(len(qs)))
		return
	}
	if size >= checkpointHardBytes {
		w.checkpointer.signal()
		if uint64(size) > w.checkpointer.completeSize.Load() {
			for i := range rs {
				rs[i].reply = failure(503, "writer unavailable")
			}
			w.stats.rejected.Add(uint64(len(qs)))
			return
		}
	}
	if e := w.beginWrite(); e != nil {
		var sqlErr sqlite3.Error
		if errors.As(e, &sqlErr) && sqlErr.Code == sqlite3.ErrBusy {
			for i := range rs {
				rs[i].reply = failure(503, "writer unavailable")
			}
		}
		return
	}
	rollback := func() {
		if _, err := w.conn.ExecContext(ctx, "ROLLBACK"); err != nil {
			w.logWriteFailure("statement", "rollback", err)
		}
		for i := range rs {
			rs[i].reply = internal
		}
	}
	for i, q := range qs {
		if _, e := w.conn.ExecContext(ctx, "SAVEPOINT request_write"); e != nil {
			w.logWriteFailure("statement", "savepoint", e)
			rollback()
			return
		}
		r, statementErr, operation := executeWriteResult(w.statements, q)
		if statementErr != nil {
			w.logWriteFailure("statement", operation, statementErr)
		}
		if r.status >= 400 {
			if _, e := w.conn.ExecContext(ctx, "ROLLBACK TO request_write"); e != nil {
				w.logWriteFailure("statement", "rollback_to_savepoint", e)
				rollback()
				return
			}
		}
		if _, e := w.conn.ExecContext(ctx, "RELEASE request_write"); e != nil {
			w.logWriteFailure("statement", "release_savepoint", e)
			rollback()
			return
		}
		rs[i].reply = r
	}
	start := time.Now()
	_, e := w.conn.ExecContext(ctx, "COMMIT")
	w.stats.commit(time.Since(start))
	if e != nil { // COMMIT may leave the transaction active; never report its successes.
		w.logWriteFailure("COMMIT", "commit", e)
		if _, rollbackErr := w.conn.ExecContext(ctx, "ROLLBACK"); rollbackErr != nil {
			w.logWriteFailure("statement", "rollback_after_commit", rollbackErr)
		}
		for i := range rs {
			if rs[i].reply.status < 400 {
				rs[i].reply = internal
			}
		}
		return
	}
	w.stats.batches.Add(1)
	w.stats.writes.Add(uint64(len(qs)))
	maximum(&w.stats.batchMax, uint64(len(qs)))
}
