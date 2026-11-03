package sqlite

import (
	"path/filepath"
	"testing"
)

func openTemp(t *testing.T) *Conn {
	t.Helper()
	path := filepath.Join(t.TempDir(), "t.db")
	c, err := Create(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

func TestPrepareStepColumns(t *testing.T) {
	c := openTemp(t)
	if err := c.Exec(`CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)`); err != nil {
		t.Fatal(err)
	}
	ins, err := c.Prepare(`INSERT INTO t (name) VALUES (?)`)
	if err != nil {
		t.Fatal(err)
	}
	defer ins.Finalize()
	for _, n := range []string{"ann", "bob"} {
		if err := ins.BindText(1, n); err != nil {
			t.Fatal(err)
		}
		if row, err := ins.Step(); err != nil || row {
			t.Fatalf("insert %s: row=%v err=%v", n, row, err)
		}
		if got := c.Changes(); got != 1 {
			t.Fatalf("changes = %d, want 1", got)
		}
		ins.Reset()
	}
	sel, err := c.Prepare(`SELECT id, name FROM t ORDER BY id`)
	if err != nil {
		t.Fatal(err)
	}
	defer sel.Finalize()
	var got []string
	for {
		row, err := sel.Step()
		if err != nil {
			t.Fatal(err)
		}
		if !row {
			break
		}
		got = append(got, string(sel.ColumnRawText(1)))
		if sel.ColumnInt64(0) != int64(len(got)) {
			t.Fatalf("id %d at position %d", sel.ColumnInt64(0), len(got))
		}
	}
	if len(got) != 2 || got[0] != "ann" || got[1] != "bob" {
		t.Fatalf("got %v", got)
	}
}

func TestConstraintErrorCode(t *testing.T) {
	c := openTemp(t)
	if err := c.Exec(`PRAGMA foreign_keys=ON; CREATE TABLE p (id INTEGER PRIMARY KEY); CREATE TABLE l (pid INTEGER REFERENCES p(id))`); err != nil {
		t.Fatal(err)
	}
	st, err := c.Prepare(`INSERT INTO l (pid) VALUES (?)`)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Finalize()
	st.BindInt64(1, 42)
	_, err = st.Step()
	if Code(err) != ErrConstraintForeignKey {
		t.Fatalf("code = %d (%v), want %d", Code(err), err, ErrConstraintForeignKey)
	}
	// The statement must be reset so it can run again after the error.
	st.Reset()
	st.BindInt64(1, 42)
	if _, err = st.Step(); Code(err) != ErrConstraintForeignKey {
		t.Fatalf("second step: %v", err)
	}
}

func TestOpenMissingFileFails(t *testing.T) {
	if _, err := Open(filepath.Join(t.TempDir(), "nope.db")); err == nil {
		t.Fatal("Open on a missing file should fail: the server must never create an empty database")
	}
}
