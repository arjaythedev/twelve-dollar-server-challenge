package main

/*
#cgo CFLAGS: -I${SRCDIR} -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0
#include <stdlib.h>
#include <stdint.h>
#include "sqlite3.h"

typedef struct BtreeReader BtreeReader;

BtreeReader *btree_reader_new(sqlite3 *db, unsigned posts_root,
                              unsigned users_root, unsigned posts_idx_root,
                              unsigned likes_idx_root);
int feed_btree(BtreeReader *r, char *out, int cap, int *outlen);
int posts_btree(BtreeReader *r, const sqlite3_int64 *ids, int n,
                char *out, int cap, int *outlen, int *lens);
void btree_stats_enable(int on);
void btree_stats(uint64_t *out);
*/
import "C"

import (
	"unsafe"
)

type btreeRoots struct {
	posts    int
	users    int
	postsIdx int
	likesIdx int
}

type btreeReader struct {
	p   *C.BtreeReader
	out []byte
}

func (b *btreeReader) ensure(n int) []byte {
	if cap(b.out) < n {
		b.out = make([]byte, n)
	} else {
		b.out = b.out[:n]
	}
	return b.out
}

type btreePost struct {
	body  []byte
	found bool
}

// loadBtreeRoots reads the fixed schema's root pages once at startup. The
// SQL path remains available if the schema is not the expected one.
func (c *sqliteConn) loadBtreeRoots() (btreeRoots, bool) {
	var r btreeRoots
	names := []struct {
		name string
		dst  *int
	}{
		{"posts", &r.posts},
		{"users", &r.users},
		{"posts_created_at_id_idx", &r.postsIdx},
		{"likes_post_id_idx", &r.likesIdx},
	}
	st, err := c.prepare("SELECT rootpage FROM sqlite_master WHERE name = ?1 AND type IN ('table','index')")
	if err != nil {
		return r, false
	}
	defer st.close()
	for _, it := range names {
		st.reset()
		if err := st.bindText(1, it.name); err != nil {
			return r, false
		}
		row, err := st.step()
		if err != nil || !row {
			return r, false
		}
		*it.dst = int(st.columnInt64(0))
		if *it.dst <= 0 {
			return r, false
		}
	}
	return r, true
}

func enableBtreeStats() { C.btree_stats_enable(1) }

func readBtreeStats() [4]uint64 {
	var out [4]C.uint64_t
	C.btree_stats((*C.uint64_t)(unsafe.Pointer(&out[0])))
	return [4]uint64{uint64(out[0]), uint64(out[1]), uint64(out[2]), uint64(out[3])}
}

func (c *sqliteConn) newBtreeReader(roots btreeRoots) *btreeReader {
	p := C.btree_reader_new(c.h,
		C.uint(roots.posts), C.uint(roots.users),
		C.uint(roots.postsIdx), C.uint(roots.likesIdx))
	if p == nil {
		return nil
	}
	return &btreeReader{p: p}
}

func (b *btreeReader) feed() ([]byte, bool) {
	out := b.ensure(32 << 10)
	var n C.int
	rc := C.feed_btree(b.p, (*C.char)(unsafe.Pointer(&out[0])), C.int(len(out)), &n)
	if rc != 0 || n < 0 || int(n) > len(out) {
		return nil, false
	}
	return out[:int(n)], true
}

func (b *btreeReader) post(id int64) ([]byte, bool, bool) {
	res, ok := b.posts([]int64{id})
	if !ok {
		return nil, false, false
	}
	return res[0].body, res[0].found, true
}

// posts returns each object fragment in the same order as ids. A missing post
// has found=false and a nil body; ok=false means the SQL fallback must run.
func (b *btreeReader) posts(ids []int64) ([]btreePost, bool) {
	if len(ids) == 0 {
		return nil, true
	}
	cids := make([]C.sqlite3_int64, len(ids))
	for i, id := range ids {
		cids[i] = C.sqlite3_int64(id)
	}
	capBytes := len(ids) * (4 << 10)
	if capBytes < 4<<10 {
		capBytes = 4 << 10
	}
	out := b.ensure(capBytes)
	lens := make([]C.int, len(ids))
	var n C.int
	rc := C.posts_btree(b.p, (*C.sqlite3_int64)(unsafe.Pointer(&cids[0])),
		C.int(len(cids)), (*C.char)(unsafe.Pointer(&out[0])),
		C.int(len(out)), &n, (*C.int)(unsafe.Pointer(&lens[0])))
	if rc != 0 || n < 0 || int(n) > len(out) {
		return nil, false
	}
	res := make([]btreePost, len(ids))
	pos := 0
	for i, l := range lens {
		if l < 0 || pos+int(l) > len(out) {
			return nil, false
		}
		if l == 0 {
			continue
		}
		res[i] = btreePost{body: out[pos : pos+int(l)], found: true}
		pos += int(l)
	}
	if pos != int(n) {
		return nil, false
	}
	return res, true
}
