package main

import (
	"runtime"

	"feedapi/internal/sqlite"
)

// runCheckpointer does the bulk of WAL checkpointing on its own thread and
// connection, so the fsync never stalls the loop. It works only on request:
// the loop asks for a pass every couple of seconds, a PASSIVE checkpoint
// copies every frame committed so far (never blocking a reader or writer), and
// done is signalled. The loop then finishes the job itself (server.boundWAL):
// a second PASSIVE on its own connection copies only the frames committed
// during this pass, which is cheap, and leaves the WAL fully backfilled with
// no reader inside it, so the loop's next write transaction restarts the WAL
// from the beginning. RESTART/TRUNCATE from this thread would instead hold
// the write lock while waiting on readers and stall the loop. wake pokes the
// loop so it notices done without waiting for the next socket event.
func runCheckpointer(db *sqlite.Conn, reqs <-chan struct{}, done chan<- struct{}, wake func(), stop <-chan struct{}, exited chan<- struct{}) {
	runtime.LockOSThread()
	defer close(exited)
	passive, err := db.Prepare("PRAGMA wal_checkpoint(PASSIVE)")
	if err != nil {
		return
	}
	defer func() { passive.Finalize(); db.Close() }()
	for {
		select {
		case <-stop:
			return
		case <-reqs:
			passive.Step()
			passive.Reset()
			select {
			case done <- struct{}{}:
			default:
			}
			if wake != nil {
				wake()
			}
		}
	}
}
