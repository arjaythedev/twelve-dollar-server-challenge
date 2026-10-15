package main

import (
	"runtime"
	"time"

	"feedapi/internal/sqlite"
)

var checkpointEvery = 2 * time.Second // a variable so tests can tick faster

// runCheckpointer copies WAL frames back into the main file on its own thread
// and connection, so the fsync never stalls the loop. Only PASSIVE mode is
// used here: it never takes a lock a reader or writer waits on. RESTART and
// TRUNCATE hold the write lock while they wait for readers, which stalls the
// loop for the whole busy timeout when run from another thread. Bounding the
// WAL file is the loop's own job (server.boundWAL): it runs a TRUNCATE between
// batches, when there is nothing to wait for, and this thread's passes keep
// that cheap.
func runCheckpointer(db *sqlite.Conn, stop <-chan struct{}, done chan<- struct{}) {
	runtime.LockOSThread()
	defer close(done)
	passive, err := db.Prepare("PRAGMA wal_checkpoint(PASSIVE)")
	if err != nil {
		return
	}
	defer func() { passive.Finalize(); db.Close() }()
	tick := time.NewTicker(checkpointEvery)
	defer tick.Stop()
	for {
		select {
		case <-stop:
			return
		case <-tick.C:
			checkpointMu.Lock()
			passive.Step()
			passive.Reset()
			checkpointMu.Unlock()
		}
	}
}
