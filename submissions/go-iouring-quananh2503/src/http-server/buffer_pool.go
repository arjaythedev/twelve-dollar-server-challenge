package httpserver

import (
	"errors"
)

var (
	ErrPoolExhausted = errors.New("bể bộ đệm đã hết slot trống")
	ErrInvalidBufID  = errors.New("bufID không hợp lệ")
)

// BufferPool quản lý một vùng nhớ arena phẳng được cấp phát trước duy nhất.
// Hoạt động hoàn toàn theo độ phức tạp O(1) và không cấp phát bộ nhớ trên Heap (0 mallocs).
type BufferPool struct {
	arena    []byte   // Vùng đệm nhớ phẳng duy nhất của toàn bộ pool
	slotSize int      // Kích thước cố định của mỗi slot (ví dụ: 4096 bytes)
	capacity int      // Tổng số slot trong pool
	freeIDs  []uint32 // Mảng chỉ mục LIFO quản lý các slot đang rảnh rỗi
	top      int      // Con trỏ đỉnh ngăn xếp LIFO
}

// NewBufferPool khởi tạo trước toàn bộ vùng nhớ arena phẳng và danh sách freeIDs
func NewBufferPool(capacity int, slotSize int) *BufferPool {
	arena := make([]byte, capacity*slotSize)
	freeIDs := make([]uint32, capacity)

	// Khởi tạo ngăn xếp LIFO: các ID từ 0 đến capacity-1
	for i := 0; i < capacity; i++ {
		freeIDs[i] = uint32(i)
	}

	return &BufferPool{
		arena:    arena,
		slotSize: slotSize,
		capacity: capacity,
		freeIDs:  freeIDs,
		top:      capacity, // Ban đầu toàn bộ slot đều rảnh rỗi
	}
}

// Acquire mượn một slot buffer từ ngăn xếp LIFO trong O(1). 0 Heap Allocation.
func (p *BufferPool) Acquire() (uint32, error) {
	if p.top == 0 {
		return 0, ErrPoolExhausted
	}
	p.top--
	return p.freeIDs[p.top], nil
}

// Release trả slot buffer về lại ngăn xếp LIFO trong O(1). 0 Heap Allocation.
func (p *BufferPool) Release(id uint32) {
	if int(id) >= p.capacity || p.top >= p.capacity {
		return
	}
	p.freeIDs[p.top] = id
	p.top++
}

// Get trả về slice toàn bộ kích thước của slot tương ứng với id. 0 Heap Allocation.
func (p *BufferPool) Get(id uint32) []byte {
	start := int(id) * p.slotSize
	return p.arena[start : start+p.slotSize]
}

// GetSlice trả về slice với độ dài length mong muốn trong slot. 0 Heap Allocation.
func (p *BufferPool) GetSlice(id uint32, length int) []byte {
	start := int(id) * p.slotSize
	return p.arena[start : start+length]
}

// Available trả về số lượng slot còn rảnh rỗi trong pool
func (p *BufferPool) Available() int {
	return p.top
}
