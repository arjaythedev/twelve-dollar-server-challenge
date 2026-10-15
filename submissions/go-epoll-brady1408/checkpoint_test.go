package main

import (
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"feedapi/internal/sqlite"
)

// A checkpoint must never make the loop wait. With a reader pinning an old WAL
// snapshot and a WAL past any size threshold, write transactions on the loop
// connection still complete in well under the busy handler's sleep steps.
func TestCheckpointerNeverBlocksWriters(t *testing.T) {
	db := openTestDB(t)
	// Find the database path through a PRAGMA so the test does not depend on openTestDB internals.
	st, _ := db.Prepare("PRAGMA database_list")
	st.Step()
	dbPath := st.ColumnText(2)
	st.Finalize()

	// Grow the WAL past 32 MB in one transaction (no checkpoint can run yet).
	big := strings.Repeat("x", 500)
	if err := db.Exec("BEGIN"); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 70000; i++ {
		if err := db.Exec(fmt.Sprintf("INSERT INTO posts (user_id, body) VALUES (1, '%s')", big)); err != nil {
			t.Fatal(err)
		}
	}
	if err := db.Exec("COMMIT"); err != nil {
		t.Fatal(err)
	}
	if fi, err := os.Stat(dbPath + "-wal"); err != nil || fi.Size() < 32<<20 {
		t.Fatalf("wal size %v err %v, want > 32 MiB", fi, err)
	}

	// A third connection holds a read transaction so a RESTART can never finish.
	reader, err := sqlite.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	if err := reader.Exec("BEGIN; SELECT count(*) FROM posts;"); err != nil {
		t.Fatal(err)
	}

	cp, err := openCheckpointConn(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	reqs, done := make(chan struct{}, 1), make(chan struct{}, 1)
	stop, exited := make(chan struct{}), make(chan struct{})
	go runCheckpointer(cp, reqs, done, nil, stop, exited)
	defer func() { close(stop); <-exited }()
	go func() { // keep asking for passes as fast as it will take them
		for {
			select {
			case <-stop:
				return
			case reqs <- struct{}{}:
				time.Sleep(time.Millisecond)
			}
		}
	}()
	time.Sleep(20 * time.Millisecond)

	s, err := newServer(db, testSecret)
	if err != nil {
		t.Fatal(err)
	}
	defer s.close()
	var worst time.Duration
	for i := 0; i < 40; i++ {
		t0 := time.Now()
		if err := s.beginTx(); err != nil {
			t.Fatal(err)
		}
		if err := db.Exec("INSERT INTO likes (user_id, post_id) VALUES (2, " + fmt.Sprint(2+i%2) + ") ON CONFLICT DO NOTHING"); err != nil {
			t.Fatal(err)
		}
		if err := s.commitBatch(); err != nil {
			t.Fatal(err)
		}
		if d := time.Since(t0); d > worst {
			worst = d
		}
		time.Sleep(3 * time.Millisecond)
	}
	if worst > 20*time.Millisecond {
		t.Fatalf("a write batch waited %v on the checkpointer; it must never block the loop", worst)
	}
}
