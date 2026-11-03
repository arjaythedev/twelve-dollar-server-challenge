package main

import (
	"database/sql/driver"
	"io"
)

// Group-commit writer.
//
// SQLite allows a single writer under WAL. Instead of a mutex per write, one
// goroutine owns the write connection and drains all currently-queued writes
// into ONE transaction, committing them together (group commit, allowed by
// rule 6). Each request still blocks until its own write is durably committed:
// the writer only signals a request's done channel AFTER the COMMIT returns.
//
// We never add artificial delay to "wait for a bigger batch" — that would hurt
// p99. We simply opportunistically absorb whatever writes already arrived
// while the previous batch was committing. Under low write load each batch is
// of size 1 (same durability as before); under bursts, fsync cost amortizes.

type writeKind uint8

const (
	kindPost writeKind = iota
	kindLike
)

type writeReq struct {
	kind writeKind
	uid  int64
	// post
	body string
	// like
	postID int64

	// results (filled by the writer goroutine before done is closed)
	newID        int64
	createdAt    string
	notFound     bool // like: post doesn't exist
	alreadyLiked bool // like: (user,post) already present
	err          error

	done chan struct{}
}

// run is the single writer goroutine.
func (wr *writer) run() {
	for first := range wr.reqs {
		// Collect this request plus any others already queued.
		batch := []*writeReq{first}
		for {
			select {
			case r := <-wr.reqs:
				batch = append(batch, r)
				if len(batch) >= 256 {
					// cap batch size so one txn doesn't grow unbounded
					goto execute
				}
			default:
				goto execute
			}
		}
	execute:
		wr.commitBatch(batch)
	}
}

// commitBatch executes every request in one transaction, then signals each.
func (wr *writer) commitBatch(batch []*writeReq) {
	// Begin one transaction for the whole batch.
	if _, err := wr.begin.Exec(nil); err != nil {
		// Could not even begin: fail all.
		for _, r := range batch {
			r.err = err
			close(r.done)
		}
		return
	}

	txErr := false
	for _, r := range batch {
		switch r.kind {
		case kindPost:
			r.execPost(wr)
		case kindLike:
			r.execLike(wr)
		}
		if r.err != nil {
			txErr = true
		}
	}

	// Commit the batch. A COMMIT failure invalidates everyone's durability.
	if _, err := wr.commit.Exec(nil); err != nil {
		_, _ = wr.rbck.Exec(nil)
		for _, r := range batch {
			if r.err == nil {
				r.err = err
			}
			close(r.done)
		}
		return
	}
	_ = txErr // individual per-statement errors already recorded on r.err
	for _, r := range batch {
		close(r.done)
	}
}

func (r *writeReq) execPost(wr *writer) {
	rows, err := wr.insP.Query([]driver.Value{r.uid, r.body})
	if err != nil {
		r.err = err
		return
	}
	dest := make([]driver.Value, 2)
	if e := rows.Next(dest); e == nil {
		r.newID = asInt(dest[0])
		r.createdAt = asStr(dest[1])
	} else {
		r.err = e
	}
	rows.Close()
}

func (r *writeReq) execLike(wr *writer) {
	// existence check
	exRows, err := wr.exists.Query([]driver.Value{r.postID})
	if err != nil {
		r.err = err
		return
	}
	exists := false
	dest := make([]driver.Value, 1)
	if e := exRows.Next(dest); e == nil {
		exists = true
	} else if e != io.EOF {
		err = e
	}
	exRows.Close()
	if err != nil {
		r.err = err
		return
	}
	if !exists {
		r.notFound = true
		return
	}
	res, err := wr.insL.Exec([]driver.Value{r.uid, r.postID})
	if err != nil {
		r.err = err
		return
	}
	n, _ := res.RowsAffected()
	r.alreadyLiked = n == 0
}

// submit enqueues a write and blocks until it is committed (or errors).
func (wr *writer) submit(r *writeReq) {
	r.done = make(chan struct{})
	wr.reqs <- r
	<-r.done
}
