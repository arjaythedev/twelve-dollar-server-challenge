package main

import (
	"os"
	"runtime"
	"time"

	"feedapi/internal/sqlite"
)

const (
	checkpointEvery = 2 * time.Second
	walRestartBytes = 32 << 20
)

// runCheckpointer copies WAL frames back into the main file on its own thread
// and connection, so the fsync never stalls the loop. PASSIVE never blocks a
// writer; RESTART (when the WAL is large) waits briefly for them, and on BUSY
// simply tries again next tick.
func runCheckpointer(db *sqlite.Conn, walPath string, stop <-chan struct{}, done chan<- struct{}) {
	runtime.LockOSThread()
	defer close(done)
	passive, err := db.Prepare("PRAGMA wal_checkpoint(PASSIVE)")
	if err != nil {
		return
	}
	restart, err := db.Prepare("PRAGMA wal_checkpoint(RESTART)")
	if err != nil {
		return
	}
	defer func() { passive.Finalize(); restart.Finalize(); db.Close() }()
	tick := time.NewTicker(checkpointEvery)
	defer tick.Stop()
	for {
		select {
		case <-stop:
			return
		case <-tick.C:
			st := passive
			if fi, err := os.Stat(walPath); err == nil && fi.Size() > walRestartBytes {
				st = restart
			}
			st.Step()
			st.Reset()
		}
	}
}
