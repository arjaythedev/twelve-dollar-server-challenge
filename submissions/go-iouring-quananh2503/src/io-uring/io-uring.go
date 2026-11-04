package iouring

import (
	"fmt"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	SYS_IO_URING_SETUP = 425
	SYS_IO_URING_ENTER = 426

	OFF_SQ_RING = 0
	OFF_CQ_RING = 0x8000000
	OFF_SQES    = 0x10000000

	IORING_ENTER_GETEVENTS = 1 << 0

	IORING_SETUP_SUBMIT_ALL    = 1 << 7
	IORING_SETUP_COOP_TASKRUN  = 1 << 8
	IORING_SETUP_SINGLE_ISSUER = 1 << 12
	IORING_SETUP_DEFER_TASKRUN = 1 << 13
)

// Các Opcode cơ bản
const (
	OpNop    uint8 = 0
	OpAccept uint8 = 13
	OpClose  uint8 = 19
	OpRead   uint8 = 22
	OpWrite  uint8 = 23
	OpSend   uint8 = 26
	OpRecv   uint8 = 27
)

// =============================================================================
// 1. HAI STRUCT DUY NHẤT CHO TẦNG TRÊN (SendInfo & ReadInfo)
// =============================================================================

// SendInfo chứa đầy đủ thông tin của 1 yêu cầu cần gửi vào Ring
type SendInfo struct {
	Opcode   uint8  // OpRead, OpWrite, OpAccept, OpClose, OpSend, OpRecv
	FD       int32  // Socket FD hoặc File FD
	Buf      []byte // Mảng byte đọc/ghi
	Off      uint64 // Offset file (socket thì để là 0)
	UserData uint64 // Tag ID do bạn tự đặt
}

// ReadInfo chứa đầy đủ thông tin của 1 kết quả hoàn thành từ Ring
type ReadInfo struct {
	UserData uint64 // Khớp với UserData lúc gửi
	Res      int32  // >= 0: số bytes hoặc FD mới; < 0: lỗi (-errno)
}

// =============================================================================
// 2. IO_URING & CÁC HÀM CHÍNH
// =============================================================================

type SQRing struct {
	head      *uint32
	tail      *uint32
	mask      uint32
	entries   uint32
	buf       []byte
	sqes      []rawSQE
	sqesMem   []byte
	localTail uint32
	toSubmit  uint32
}

type CQRing struct {
	head    *uint32
	tail    *uint32
	mask    uint32
	entries uint32
	buf     []byte
	cqes    []rawCQE
}

type IOUring struct {
	fd int
	sq SQRing
	cq CQRing
}

// NewRing khởi tạo ring với các cờ tối ưu nhân Linux (SINGLE_ISSUER + DEFER_TASKRUN)
func NewRing(size int) (*IOUring, error) {
	var params RingParams
	params.Flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL

	fd, _, errno := syscall.Syscall6(
		SYS_IO_URING_SETUP,
		uintptr(size),
		uintptr(unsafe.Pointer(&params)),
		0, 0, 0, 0,
	)
	if errno != 0 {
		return nil, fmt.Errorf("io_uring_setup(%d) failed, errno: %d", size, errno)
	}
	ring := IOUring{}
	ring.fd = int(fd)

	prot := syscall.PROT_READ | syscall.PROT_WRITE
	flags := syscall.MAP_SHARED | syscall.MAP_POPULATE

	// 1. SQ Ring
	sqSize := params.SQOff.Array + params.SQEntries*4
	sqMem, err := syscall.Mmap(ring.fd, OFF_SQ_RING, int(sqSize), prot, flags)
	if err != nil {
		syscall.Close(ring.fd)
		return nil, fmt.Errorf("mmap SQ ring error: %w", err)
	}
	ring.sq.buf = sqMem
	ring.sq.entries = params.SQEntries
	ring.sq.head = (*uint32)(unsafe.Pointer(&sqMem[params.SQOff.Head]))
	ring.sq.tail = (*uint32)(unsafe.Pointer(&sqMem[params.SQOff.Tail]))
	ring.sq.mask = *(*uint32)(unsafe.Pointer(&sqMem[params.SQOff.RingMask]))
	ring.sq.localTail = atomic.LoadUint32(ring.sq.tail)

	sqArray := unsafe.Slice((*uint32)(unsafe.Pointer(&sqMem[params.SQOff.Array])), params.SQEntries)
	for i := range sqArray {
		sqArray[i] = uint32(i)
	}

	// 2. Mảng SQEs
	sqesSize := int(params.SQEntries) * 64
	sqesMem, err := syscall.Mmap(ring.fd, OFF_SQES, sqesSize, prot, flags)
	if err != nil {
		ring.Close()
		return nil, fmt.Errorf("mmap SQEs error: %w", err)
	}
	ring.sq.sqesMem = sqesMem
	ring.sq.sqes = unsafe.Slice((*rawSQE)(unsafe.Pointer(&sqesMem[0])), params.SQEntries)

	// 3. CQ Ring
	cqSize := params.CQOff.CQEs + params.CQEntries*16
	cqMem, err := syscall.Mmap(ring.fd, OFF_CQ_RING, int(cqSize), prot, flags)
	if err != nil {
		ring.Close()
		return nil, fmt.Errorf("mmap CQ ring error: %w", err)
	}
	ring.cq.buf = cqMem
	ring.cq.entries = params.CQEntries
	ring.cq.head = (*uint32)(unsafe.Pointer(&cqMem[params.CQOff.Head]))
	ring.cq.tail = (*uint32)(unsafe.Pointer(&cqMem[params.CQOff.Tail]))
	ring.cq.mask = *(*uint32)(unsafe.Pointer(&cqMem[params.CQOff.RingMask]))
	ring.cq.cqes = unsafe.Slice((*rawCQE)(unsafe.Pointer(&cqMem[params.CQOff.CQEs])), params.CQEntries)

	return &ring, nil
}

// SendTasks: Nhận vào 1 mảng SendInfo, điền toàn bộ vào SQ và gọi Syscall gửi sang Kernel cùng lúc!
// Đảm bảo nộp toàn bộ requests (chia làm nhiều đợt nếu kích thước lớn hơn SQ entries).
func (io *IOUring) SendTasks(requests []SendInfo) (int, error) {
	totalSubmitted := 0
	i := 0

	for i < len(requests) {
		head := atomic.LoadUint32(io.sq.head)

		for i < len(requests) && io.sq.localTail-head < io.sq.entries {
			req := requests[i]
			s := &io.sq.sqes[io.sq.localTail&io.sq.mask]
			*s = rawSQE{
				Opcode:   req.Opcode,
				FD:       req.FD,
				Off:      req.Off,
				UserData: req.UserData,
			}
			if len(req.Buf) > 0 {
				s.Addr = uint64(uintptr(unsafe.Pointer(&req.Buf[0])))
				s.Len = uint32(len(req.Buf))
			}

			io.sq.localTail++
			io.sq.toSubmit++
			i++
		}

		if io.sq.toSubmit > 0 {
			atomic.StoreUint32(io.sq.tail, io.sq.localTail)

			n, _, errno := syscall.Syscall6(
				SYS_IO_URING_ENTER,
				uintptr(io.fd),
				uintptr(io.sq.toSubmit),
				0,
				uintptr(IORING_ENTER_GETEVENTS),
				0, 0,
			)
			if errno != 0 && errno != syscall.EINTR && errno != syscall.EAGAIN {
				return totalSubmitted, errno
			}

			io.sq.toSubmit -= uint32(n)
			totalSubmitted += int(n)
		}
	}

	return totalSubmitted, nil
}

// RecieveOutputTasks: Nhận vào mảng out []ReadInfo, gom tất cả kết quả hoàn thành từ CQ đổ vào mảng này
// waitMin: số lượng kết quả tối thiểu muốn chờ (0 = không chờ; >=1: chờ có kết quả)
func (io *IOUring) RecieveOutputTasks(out []ReadInfo, waitMin uint32) (int, error) {
	head := *io.cq.head
	tail := atomic.LoadUint32(io.cq.tail)

	if head == tail && waitMin > 0 {
		_, _, errno := syscall.Syscall6(
			SYS_IO_URING_ENTER,
			uintptr(io.fd),
			0,
			uintptr(waitMin),
			uintptr(IORING_ENTER_GETEVENTS),
			0, 0,
		)
		if errno != 0 && errno != syscall.EINTR {
			return 0, errno
		}
		tail = atomic.LoadUint32(io.cq.tail)
	}

	count := 0
	maxCap := len(out)

	for head != tail && count < maxCap {
		raw := &io.cq.cqes[head&io.cq.mask]
		out[count] = ReadInfo{
			UserData: raw.UserData,
			Res:      raw.Res,
		}
		count++
		head++
	}

	if count > 0 {
		atomic.StoreUint32(io.cq.head, head)
	}

	return count, nil
}

// SubmitAndRecieve: Unified Single-Syscall nộp lô requests mới (SQ) VÀ gặt kết quả (CQ)
// Giảm 50% chi phí gọi Syscall, tự động tận dụng Natural Batching và 0-syscall khi RAM đã có sẵn việc!
func (io *IOUring) SubmitAndRecieve(requests []SendInfo, out []ReadInfo, waitMin uint32) (int, error) {
	// 1. Điền toàn bộ requests vào Submission Queue (SQ) trên RAM
	i := 0
	for i < len(requests) {
		head := atomic.LoadUint32(io.sq.head)

		for i < len(requests) && io.sq.localTail-head < io.sq.entries {
			req := requests[i]
			s := &io.sq.sqes[io.sq.localTail&io.sq.mask]
			*s = rawSQE{
				Opcode:   req.Opcode,
				FD:       req.FD,
				Off:      req.Off,
				UserData: req.UserData,
			}
			if len(req.Buf) > 0 {
				s.Addr = uint64(uintptr(unsafe.Pointer(&req.Buf[0])))
				s.Len = uint32(len(req.Buf))
			}

			io.sq.localTail++
			io.sq.toSubmit++
			i++
		}

		// Nếu danh sách nộp quá lớn vượt quá kích thước Ring, xả bớt trước (minComplete=0)
		if i < len(requests) && io.sq.toSubmit > 0 {
			atomic.StoreUint32(io.sq.tail, io.sq.localTail)
			n, _, errno := syscall.Syscall6(
				SYS_IO_URING_ENTER,
				uintptr(io.fd),
				uintptr(io.sq.toSubmit),
				0,
				uintptr(IORING_ENTER_GETEVENTS),
				0, 0,
			)
			if errno != 0 && errno != syscall.EINTR && errno != syscall.EAGAIN {
				return 0, errno
			}
			io.sq.toSubmit -= uint32(n)
		}
	}

	if io.sq.toSubmit > 0 {
		atomic.StoreUint32(io.sq.tail, io.sq.localTail)
	}

	// 2. Kiểm tra hàng đợi hoàn thành (CQ)
	cqHead := *io.cq.head
	cqTail := atomic.LoadUint32(io.cq.tail)

	// 3. Gọi ĐÚNG 1 SYSCALL DUY NHẤT nếu có việc cần Submit HOẶC CQ đang rỗng cần Wait
	flags := uint32(IORING_ENTER_GETEVENTS)
	minComplete := uint32(0)
	if cqHead == cqTail && waitMin > 0 {
		minComplete = waitMin
	}

	for io.sq.toSubmit > 0 || (cqHead == cqTail && minComplete > 0) {
		n, _, errno := syscall.Syscall6(
			SYS_IO_URING_ENTER,
			uintptr(io.fd),
			uintptr(io.sq.toSubmit),
			uintptr(minComplete),
			uintptr(flags),
			0, 0,
		)
		if errno == syscall.EINTR {
			continue
		}
		if errno != 0 && errno != syscall.EAGAIN {
			return 0, errno
		}
		io.sq.toSubmit -= uint32(n)
		cqTail = atomic.LoadUint32(io.cq.tail)
		minComplete = 0
	}

	// 4. Thu hoạch (Drain) toàn bộ kết quả đã xong trong CQ Ring trên RAM
	count := 0
	maxCap := len(out)

	for cqHead != cqTail && count < maxCap {
		raw := &io.cq.cqes[cqHead&io.cq.mask]
		out[count] = ReadInfo{
			UserData: raw.UserData,
			Res:      raw.Res,
		}
		count++
		cqHead++
	}

	if count > 0 {
		atomic.StoreUint32(io.cq.head, cqHead)
	}

	return count, nil
}

// Close giải phóng toàn bộ 3 vùng nhớ mmap và đóng file descriptor của Ring
func (io *IOUring) Close() error {
	var firstErr error

	// 1. Munmap giải phóng 3 vùng RAM dùng chung với Kernel
	for _, m := range [][]byte{io.sq.sqesMem, io.cq.buf, io.sq.buf} {
		if m != nil {
			if err := syscall.Munmap(m); err != nil && firstErr == nil {
				firstErr = err
			}
		}
	}
	io.sq.sqesMem, io.cq.buf, io.sq.buf = nil, nil, nil

	// 2. Đóng File Descriptor của io_uring
	if io.fd > 0 {
		if err := syscall.Close(io.fd); err != nil && firstErr == nil {
			firstErr = err
		}
		io.fd = -1 // Đánh dấu đã đóng, không đóng lại lần 2
	}

	return firstErr
}

// =============================================================================
// CẤU TRÚC NỘI BỘ LINUX KERNEL (Ẩn bên dưới)
// =============================================================================

type rawCQE struct {
	UserData uint64
	Res      int32
	Flags    uint32
}

type rawSQE struct {
	Opcode      uint8
	Flags       uint8
	IOPrio      uint16
	FD          int32
	Off         uint64
	Addr        uint64
	Len         uint32
	OpFlags     uint32
	UserData    uint64
	BufIndex    uint16
	Personality uint16
	SpliceFDIn  int32
	Addr3       uint64
	Pad2        uint64
}

type SQRingOffsets struct {
	Head, Tail, RingMask, RingEntries uint32
	Flags, Dropped, Array, Resv1      uint32
	UserAddr                          uint64
}

type CQRingOffsets struct {
	Head, Tail, RingMask, RingEntries uint32
	Overflow, CQEs, Flags, Resv1      uint32
	UserAddr                          uint64
}

type RingParams struct {
	SQEntries    uint32
	CQEntries    uint32
	Flags        uint32
	SQThreadCPU  uint32
	SQThreadIdle uint32
	Features     uint32
	WqFD         uint32
	Resv         [3]uint32
	SQOff        SQRingOffsets
	CQOff        CQRingOffsets
}
