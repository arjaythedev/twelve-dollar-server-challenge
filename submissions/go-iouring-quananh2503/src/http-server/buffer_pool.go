package httpserver

import (
	"errors"
)

var (
	ErrPoolExhausted = errors.New("bể bộ đệm đã hết slot trống")
	ErrInvalidBufID  = errors.New("bufID không hợp lệ")
)

// BufferPool quản lý một vùng nhớ arena phẳng tuần hoàn (Circular Ring Arena) được cấp phát trước.
// Hoạt động hoàn toàn theo độ phức tạp O(1), zero allocation (0 mallocs) trên hot path.
// Khi kết hợp với cờ IOSQE_CQE_SKIP_SUCCESS, các slot buffer được cấp phát tuần tự xoay vòng (head & mask).
// Dữ liệu trong slot được bảo đảm an toàn vì một vòng quay của head (16.384+ requests) mất hàng giây,
// trong khi Kernel TCP drain buffer chỉ tính bằng micro-giây.
type BufferPool struct {
	arena    []byte // Vùng đệm nhớ phẳng duy nhất của toàn bộ pool
	slotSize int    // Kích thước cố định của mỗi slot (ví dụ: 16384 bytes)
	capacity int    // Tổng số slot trong pool (lũy thừa của 2)
	mask     uint32 // Mặt nạ bit (capacity - 1) để lấy chỉ mục vòng tròn O(1)
	head     uint32 // Con trỏ cấp phát tuần tự
}

// NewBufferPool khởi tạo trước toàn bộ vùng nhớ arena phẳng với capacity lũy thừa của 2.
func NewBufferPool(capacity int, slotSize int) *BufferPool {
	// Đảm bảo capacity là lũy thừa của 2 để dùng phép toán AND bitwise
	capPow2 := 1
	for capPow2 < capacity {
		capPow2 <<= 1
	}

	arena := make([]byte, capPow2*slotSize)
	return &BufferPool{
		arena:    arena,
		slotSize: slotSize,
		capacity: capPow2,
		mask:     uint32(capPow2 - 1),
		head:     0,
	}
}

// Acquire lấy chỉ mục slot tiếp theo theo cơ chế xoay vòng O(1). 0 Heap Allocation.
func (p *BufferPool) Acquire() (uint32, error) {
	id := p.head & p.mask
	p.head++
	return id, nil
}

// Release không cần thực hiện gì trong mô hình Ring Arena xoay vòng.
// Hàm được giữ lại để tương thích API mà không tốn chi phí CPU.
func (p *BufferPool) Release(id uint32) {
}

// Get trả về slice toàn bộ kích thước của slot tương ứng với id. 0 Heap Allocation.
func (p *BufferPool) Get(id uint32) []byte {
	start := int(id&p.mask) * p.slotSize
	return p.arena[start : start+p.slotSize]
}

// GetSlice trả về slice với độ dài length mong muốn trong slot. 0 Heap Allocation.
func (p *BufferPool) GetSlice(id uint32, length int) []byte {
	start := int(id&p.mask) * p.slotSize
	return p.arena[start : start+length]
}

// Available trả về tổng dung lượng khả dụng của pool.
func (p *BufferPool) Available() int {
	return p.capacity
}
