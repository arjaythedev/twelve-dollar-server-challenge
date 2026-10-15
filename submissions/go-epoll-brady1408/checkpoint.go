package main

import (
	"runtime"
	"time"

	"feedapi/internal/sqlite"
)

var checkpointEvery = 2 * time.Second // a variable so tests can tick faster

// runCheckpointer copies WAL frames back into the main file on its own thread
// and connection, so the fsync never stalls the loop. Only PASSIVE mode is
// used: it never takes a lock a reader or writer waits on. RESTART/TRUNCATE
// hold the write lock while they wait for readers, which stalls the loop for
// the whole busy timeout. Once a PASSIVE checkpoint has caught up and no
// reader is inside the WAL, SQLite's next writer restarts the WAL from the
// beginning by itself, so the file stays bounded without a blocking mode.
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
			passive.Step()
			passive.Reset()
		}
	}
}
