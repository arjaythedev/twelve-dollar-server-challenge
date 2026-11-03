package sqlite

/*
#cgo CFLAGS: -O3 -march=native -I${SRCDIR} -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0
#include "slim.h"
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"sync"
	"unsafe"
)

var (
	feedMu sync.Mutex
	buf    = make([]byte, 65536)
)

// InitFeedEngine mở SQLite và khởi tạo các con trỏ B-tree
func InitFeedEngine(dbPath string) error {
	feedMu.Lock()
	defer feedMu.Unlock()

	cpath := C.CString(dbPath)
	defer C.free(unsafe.Pointer(cpath))

	rc := C.init_feed_engine(cpath)
	if rc != 0 {
		return errors.New("init_feed_engine thất bại")
	}
	return nil
}

// GetFeed đọc 20 post mới nhất trực tiếp từ B-tree và trả về JSON byte slice
func GetFeed() ([]byte, error) {
	feedMu.Lock()
	defer feedMu.Unlock()

	var outLen C.int
	rc := C.get_feed(
		(*C.char)(unsafe.Pointer(&buf[0])),
		C.int(len(buf)),
		&outLen,
	)

	if rc != 0 {
		return nil, errors.New("get_feed thất bại")
	}

	res := make([]byte, int(outLen))
	copy(res, buf[:outLen])
	return res, nil
}

// GetFeedInto viết trực tiếp vào buffer out slice để đạt zero-allocation
func GetFeedInto(out []byte) (int, error) {
	feedMu.Lock()
	defer feedMu.Unlock()

	if len(out) == 0 {
		return 0, errors.New("buffer rỗng")
	}

	var outLen C.int
	rc := C.get_feed(
		(*C.char)(unsafe.Pointer(&out[0])),
		C.int(len(out)),
		&outLen,
	)

	if rc != 0 {
		return 0, errors.New("get_feed thất bại")
	}

	return int(outLen), nil
}

// GetPost đọc bài viết theo id trực tiếp từ B-tree
// Trả về: (jsonBytes, statusCode, error)
func GetPost(id int64) ([]byte, int, error) {
	feedMu.Lock()
	defer feedMu.Unlock()

	var outLen C.int
	status := int(C.get_post(
		C.int64_t(id),
		(*C.char)(unsafe.Pointer(&buf[0])),
		C.int(len(buf)),
		&outLen,
	))

	if status == 404 {
		return nil, 404, nil
	}
	if status != 200 {
		return nil, 500, errors.New("get_post thất bại")
	}

	res := make([]byte, int(outLen))
	copy(res, buf[:outLen])
	return res, 200, nil
}

// PostExists kiểm tra bài viết id có tồn tại không
func PostExists(id int64) (bool, error) {
	feedMu.Lock()
	defer feedMu.Unlock()

	rc := int(C.post_exists(C.int64_t(id)))
	if rc < 0 {
		return false, errors.New("post_exists thất bại")
	}
	return rc == 1, nil
}

// PostBatchItem chứa kết quả cho từng phần tử trong batch
type PostBatchItem struct {
	Status int32
	Offset int32
	Len    int32
}

// GetPostsBatch đọc một batch bài viết theo mảng ids trong 1 CGO call & 1 transaction duy nhất
func GetPostsBatch(ids []int64, outBuf []byte, items []PostBatchItem) error {
	feedMu.Lock()
	defer feedMu.Unlock()

	n := len(ids)
	if n == 0 {
		return nil
	}

	rc := C.get_posts_batch(
		(*C.int64_t)(unsafe.Pointer(&ids[0])),
		C.int(n),
		(*C.char)(unsafe.Pointer(&outBuf[0])),
		C.int(len(outBuf)),
		(*C.PostBatchItem)(unsafe.Pointer(&items[0])),
	)

	if rc != 0 {
		return errors.New("get_posts_batch thất bại")
	}
	return nil
}

// PostExistsBatch kiểm tra tồn tại của một batch bài viết trong 1 CGO call & 1 transaction duy nhất
func PostExistsBatch(ids []int64, results []int32) error {
	feedMu.Lock()
	defer feedMu.Unlock()

	n := len(ids)
	if n == 0 {
		return nil
	}

	rc := C.post_exists_batch(
		(*C.int64_t)(unsafe.Pointer(&ids[0])),
		C.int(n),
		(*C.int32_t)(unsafe.Pointer(&results[0])),
	)

	if rc != 0 {
		return errors.New("post_exists_batch thất bại")
	}
	return nil
}

// DBHealth kiểm tra database còn hoạt động hay không
func DBHealth() bool {
	feedMu.Lock()
	defer feedMu.Unlock()
	return C.db_health() == 1
}

// CreatePostBatchItem chứa kết quả cho từng bài viết tạo mới trong batch
type CreatePostBatchItem struct {
	ID        int64
	Status    int32
	CatOffset int32
	CatLen    int32
}

// CreatePostsBatch tạo một batch bài viết mới trong 1 CGO call & 1 transaction duy nhất (Group Commit)
func CreatePostsBatch(
	userIDs []int64,
	bodiesBuf []byte,
	bodyOffsets, bodyLens []int32,
	outCatBuf []byte,
	items []CreatePostBatchItem,
) error {
	feedMu.Lock()
	defer feedMu.Unlock()

	n := len(userIDs)
	if n == 0 {
		return nil
	}

	var bodiesPtr *C.char
	if len(bodiesBuf) > 0 {
		bodiesPtr = (*C.char)(unsafe.Pointer(&bodiesBuf[0]))
	}

	var catPtr *C.char
	if len(outCatBuf) > 0 {
		catPtr = (*C.char)(unsafe.Pointer(&outCatBuf[0]))
	}

	rc := C.create_posts_batch(
		(*C.int64_t)(unsafe.Pointer(&userIDs[0])),
		bodiesPtr,
		(*C.int32_t)(unsafe.Pointer(&bodyOffsets[0])),
		(*C.int32_t)(unsafe.Pointer(&bodyLens[0])),
		C.int(n),
		catPtr,
		C.int(len(outCatBuf)),
		(*C.CreatePostBatchItem)(unsafe.Pointer(&items[0])),
	)

	if rc != 0 {
		return errors.New("create_posts_batch thất bại")
	}
	return nil
}

// InsertLikesBatch chèn một batch lượt like trong 1 CGO call & 1 transaction duy nhất (Group Commit)
// results: 201 (vừa like), 200 (đã like trước đó), 404 (post không tồn tại), 500 (lỗi)
func InsertLikesBatch(userIDs, postIDs []int64, results []int32) error {
	feedMu.Lock()
	defer feedMu.Unlock()

	n := len(userIDs)
	if n == 0 {
		return nil
	}

	rc := C.insert_likes_batch(
		(*C.int64_t)(unsafe.Pointer(&userIDs[0])),
		(*C.int64_t)(unsafe.Pointer(&postIDs[0])),
		C.int(n),
		(*C.int32_t)(unsafe.Pointer(&results[0])),
	)

	if rc != 0 {
		return errors.New("insert_likes_batch thất bại")
	}
	return nil
}

// CloseFeedEngine giải phóng B-tree engine
func CloseFeedEngine() {
	feedMu.Lock()
	defer feedMu.Unlock()
	C.close_feed_engine()
}
