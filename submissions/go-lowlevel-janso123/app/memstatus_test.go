//go:build cgo_sqlite && !pure_sqlite

package main

import (
	"os"
	"os/exec"
	"strings"
	"testing"
)

// Configuration is process-global, so use a fresh process before any test can
// open a SQLite connection and implicitly call sqlite3_initialize.
func TestSQLiteMemoryStatusConfiguredBeforeOpen(t *testing.T) {
	if os.Getenv("FEED_FAST_MEMSTATUS_CHILD") == "1" {
		if err := configureSQLiteMemoryStatus(); err != nil {
			t.Fatal(err)
		}
		return
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestSQLiteMemoryStatusConfiguredBeforeOpen$")
	cmd.Env = append(os.Environ(), "FEED_FAST_MEMSTATUS_CHILD=1")
	output, err := cmd.CombinedOutput()
	if err != nil || !strings.Contains(string(output), "PASS") {
		t.Fatalf("fresh-process sqlite3_config: %v\n%s", err, output)
	}
}
