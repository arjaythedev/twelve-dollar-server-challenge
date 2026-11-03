// Added for the twelve-dollar-server-challenge submission.
//
// A batched "step + read all 5 columns of the post projection in a SINGLE cgo
// call" helper. The generic (*SQLiteRows).Next makes one cgo call per column
// (sqlite3_column_type, then sqlite3_column_text/_int64, then _bytes, ...),
// boxes each value into a driver.Value interface, and runs decltype/timestamp
// logic. For the hot /feed and /posts/:id reads that is ~100 cgo crossings and
// many allocations per feed response.
//
// This file lives inside package sqlite3 so it can touch the unexported
// *C.sqlite3_stmt handle. It assumes the post projection column layout:
//   0:int id, 1:text body, 2:text created_at, 3:text username, 4:int like_count
// Text pointers are copied into Go strings immediately (valid only until the
// next step), so the returned PostRow owns its data.

package sqlite3

/*
#include <stdlib.h>
#include <string.h>
#include "sqlite3-binding.h"

// Steps once and, on SQLITE_ROW, extracts the 5 post-projection columns in a
// single cgo crossing. Returns the sqlite3_step return code. Text column
// pointers/lengths are written out for the caller to copy into Go strings.
static int step_post5(sqlite3_stmt *stmt,
                      long long *id, long long *like_count,
                      const unsigned char **body, int *body_n,
                      const unsigned char **created, int *created_n,
                      const unsigned char **author, int *author_n) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        return rc;
    }
    *id         = sqlite3_column_int64(stmt, 0);
    *body       = sqlite3_column_text(stmt, 1);
    *body_n     = sqlite3_column_bytes(stmt, 1);
    *created    = sqlite3_column_text(stmt, 2);
    *created_n  = sqlite3_column_bytes(stmt, 2);
    *author     = sqlite3_column_text(stmt, 3);
    *author_n   = sqlite3_column_bytes(stmt, 3);
    *like_count = sqlite3_column_int64(stmt, 4);
    return rc;
}
*/
import "C"

import (
	"unsafe"
)

// PostRow is the decoded post projection, owning its string data.
type PostRow struct {
	ID        int64
	Body      string
	CreatedAt string
	Author    string
	LikeCount int64
}

// StepPost5 steps the statement once and decodes the 5-column post projection
// in a single cgo call. Returns (row, true, nil) on a row, (_, false, nil) at
// end of result set (io.EOF semantics), or (_, false, err) on error.
//
// The statement must have been reset/bound by the caller (use Reset + Exec of
// bindings, or re-Query). This method is NOT safe for concurrent use on the
// same statement; the submission's connection pool guarantees single-goroutine
// access per connection.
func (s *SQLiteStmt) StepPost5() (PostRow, bool, error) {
	var (
		id, likeCount          C.longlong
		bodyP, createdP, authP *C.uchar
		bodyN, createdN, authN C.int
	)
	rc := C.step_post5(s.s,
		&id, &likeCount,
		&bodyP, &bodyN,
		&createdP, &createdN,
		&authP, &authN)

	switch rc {
	case C.SQLITE_ROW:
		row := PostRow{
			ID:        int64(id),
			Body:      C.GoStringN((*C.char)(unsafe.Pointer(bodyP)), bodyN),
			CreatedAt: C.GoStringN((*C.char)(unsafe.Pointer(createdP)), createdN),
			Author:    C.GoStringN((*C.char)(unsafe.Pointer(authP)), authN),
			LikeCount: int64(likeCount),
		}
		return row, true, nil
	case C.SQLITE_DONE:
		return PostRow{}, false, nil
	default:
		return PostRow{}, false, s.c.lastError()
	}
}

// ResetStmt resets the prepared statement so it can be re-stepped. For queries
// with no bound parameters (the feed), call this before re-running StepPost5
// from the top.
func (s *SQLiteStmt) ResetStmt() error {
	rc := C.sqlite3_reset(s.s)
	if rc != C.SQLITE_OK {
		return s.c.lastError()
	}
	return nil
}

// BindAndReset resets then binds a single int64 parameter at position 1 (the
// /posts/:id query). Clears previous bindings first.
func (s *SQLiteStmt) BindInt1Reset(v int64) error {
	if rc := C.sqlite3_reset(s.s); rc != C.SQLITE_OK {
		return s.c.lastError()
	}
	if rc := C.sqlite3_clear_bindings(s.s); rc != C.SQLITE_OK {
		return s.c.lastError()
	}
	if rc := C.sqlite3_bind_int64(s.s, 1, C.longlong(v)); rc != C.SQLITE_OK {
		return s.c.lastError()
	}
	return nil
}
