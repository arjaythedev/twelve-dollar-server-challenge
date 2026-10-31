//go:build cgo_sqlite && !pure_sqlite

package main

import "testing"

// Fails if external linkage silently selects another SQLite, or changes the
// bundled feature/concurrency contract. Run in both bundled and external modes.
func TestSQLiteBuildContract(t *testing.T) {
	a, _ := fixture(t, testSchema)
	var version, source string
	if err := a.conn.QueryRowContext(ctx, "SELECT sqlite_version(), sqlite_source_id()").Scan(&version, &source); err != nil {
		t.Fatal(err)
	}
	if version != "3.53.4" || source != "2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc" {
		t.Fatalf("unexpected SQLite: %s %s", version, source)
	}
	// SQLite does not expose TRACE_SIZE_LIMIT via compileoption_used; its
	// definition is pinned and read directly by sqlite-pgo.sh.
	for _, option := range []string{"THREADSAFE=1", "ENABLE_RTREE", "ENABLE_FTS3", "ENABLE_FTS3_PARENTHESIS", "OMIT_DEPRECATED", "DEFAULT_WAL_SYNCHRONOUS=1", "ENABLE_UPDATE_DELETE_LIMIT"} {
		var enabled int
		if err := a.conn.QueryRowContext(ctx, "SELECT sqlite_compileoption_used(?)", option).Scan(&enabled); err != nil || enabled != 1 {
			t.Fatalf("SQLite option %s: enabled=%d error=%v", option, enabled, err)
		}
	}
	var omitted int
	if err := a.conn.QueryRowContext(ctx, "SELECT sqlite_compileoption_used('OMIT_DESERIALIZE')").Scan(&omitted); err != nil || omitted != 0 {
		t.Fatalf("bundled serialization API omitted: %d %v", omitted, err)
	}
}
