package httpserver

import (
	"fmt"
	"log"
	"net"
	"runtime"
	"strconv"
	"syscall"

	iouring "go-iouring-quananh2503/src/io-uring"
)

type Server struct {
	cfg       Config
	listenFDs [NumRings]int
	rings     [NumRings]*iouring.IOUring
	bufRings  [NumRings]*iouring.ProvidedBufRing
	eventFD   int
	epfd      int
	db        *DB
	sendPool  *BufferPool
	batch     *EventBatch
	tasks     []iouring.ReadInfo
	outTasks  [NumRings][]iouring.SendInfo
	parser    *Parser
	sysMgr    *SystemManager
}

func NewServer(cfg Config) (*Server, error) {
	// 1. Khởi tạo DB SQLite
	db, err := OpenDB(cfg.DBPath)
	if err != nil {
		return nil, fmt.Errorf("khởi tạo DB lỗi: %w", err)
	}

	port, err := strconv.Atoi(cfg.Port)
	if err != nil {
		port = 3000
	}

	var ip4 [4]byte
	parsedIP := net.ParseIP(cfg.Host)
	if parsedIP != nil {
		if to4 := parsedIP.To4(); to4 != nil {
			copy(ip4[:], to4)
		}
	}

	sa := &syscall.SockaddrInet4{
		Port: port,
		Addr: ip4,
	}

	// 2. Tạo 1 eventfd dùng chung (non-blocking, cloexec)
	efd, _, errno := syscall.Syscall(syscall.SYS_EVENTFD2, 0, uintptr(syscall.O_NONBLOCK|syscall.O_CLOEXEC), 0)
	if errno != 0 {
		db.Close()
		return nil, fmt.Errorf("tạo eventfd lỗi: %w", errno)
	}
	eventFD := int(efd)

	// 3. Tạo epoll để theo dõi eventfd khi các rings nhàn rỗi
	epfd, err := syscall.EpollCreate1(syscall.EPOLL_CLOEXEC)
	if err != nil {
		syscall.Close(eventFD)
		db.Close()
		return nil, fmt.Errorf("tạo epoll lỗi: %w", err)
	}

	ev := syscall.EpollEvent{
		Events: syscall.EPOLLIN,
		Fd:     int32(eventFD),
	}
	if err := syscall.EpollCtl(epfd, syscall.EPOLL_CTL_ADD, eventFD, &ev); err != nil {
		syscall.Close(epfd)
		syscall.Close(eventFD)
		db.Close()
		return nil, fmt.Errorf("epoll_ctl add eventfd lỗi: %w", err)
	}

	var listenFDs [NumRings]int
	var rings [NumRings]*iouring.IOUring
	var bufRings [NumRings]*iouring.ProvidedBufRing

	for r := 0; r < NumRings; r++ {
		// Socket lắng nghe TCP non-blocking có SO_REUSEADDR và SO_REUSEPORT (0xf trên Linux)
		lfd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
		if err != nil {
			cleanupPartial(db, eventFD, epfd, listenFDs[:r], rings[:r], bufRings[:r])
			return nil, fmt.Errorf("tạo socket ring %d lỗi: %w", r, err)
		}

		_ = syscall.SetsockoptInt(lfd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		_ = syscall.SetsockoptInt(lfd, syscall.SOL_SOCKET, 0xf, 1) // SO_REUSEPORT

		if err := syscall.SetNonblock(lfd, true); err != nil {
			syscall.Close(lfd)
			cleanupPartial(db, eventFD, epfd, listenFDs[:r], rings[:r], bufRings[:r])
			return nil, fmt.Errorf("set nonblock listenFD ring %d lỗi: %w", r, err)
		}

		if err := syscall.Bind(lfd, sa); err != nil {
			syscall.Close(lfd)
			cleanupPartial(db, eventFD, epfd, listenFDs[:r], rings[:r], bufRings[:r])
			return nil, fmt.Errorf("bind socket ring %d lỗi: %w", r, err)
		}

		if err := syscall.Listen(lfd, 4096); err != nil {
			syscall.Close(lfd)
			cleanupPartial(db, eventFD, epfd, listenFDs[:r], rings[:r], bufRings[:r])
			return nil, fmt.Errorf("listen socket ring %d lỗi: %w", r, err)
		}
		listenFDs[r] = lfd

		// Khởi tạo Ring io_uring (dung lượng 4096 entries)
		ring, err := iouring.NewRing(4096)
		if err != nil {
			cleanupPartial(db, eventFD, epfd, listenFDs[:r+1], rings[:r], bufRings[:r])
			return nil, fmt.Errorf("khởi tạo io_uring ring %d lỗi: %w", r, err)
		}
		rings[r] = ring

		// Đăng ký Fixed Files Table (65,535 slots)
		if err := iouring.RegisterFileTable(ring.FD(), 65535); err != nil {
			cleanupPartial(db, eventFD, epfd, listenFDs[:r+1], rings[:r+1], bufRings[:r])
			return nil, fmt.Errorf("đăng ký fixed files table ring %d lỗi: %w", r, err)
		}

		// Đăng ký Provided Buffer Ring (8,192 buffers x 2048 bytes)
		bufRing, err := iouring.NewProvidedBufRing(ring.FD(), 0, 8192, 2048)
		if err != nil {
			cleanupPartial(db, eventFD, epfd, listenFDs[:r+1], rings[:r+1], bufRings[:r])
			return nil, fmt.Errorf("đăng ký provided buffer ring ring %d lỗi: %w", r, err)
		}
		bufRings[r] = bufRing

		// Đăng ký eventfd vào ring r
		if err := iouring.RegisterEventFD(ring.FD(), eventFD); err != nil {
			cleanupPartial(db, eventFD, epfd, listenFDs[:r+1], rings[:r+1], bufRings[:r+1])
			return nil, fmt.Errorf("đăng ký eventfd vào ring %d lỗi: %w", r, err)
		}

		// Mồi 1 lệnh Multishot Accept ban đầu với IORING_FILE_INDEX_ALLOC
		initAccept := iouring.SendInfo{
			Opcode:     iouring.OpAccept,
			FD:         int32(lfd),
			IOPrio:     iouring.IORING_ACCEPT_MULTISHOT,
			SpliceFDIn: iouring.IORING_FILE_INDEX_ALLOC,
			UserData:   EncodeUserData(EventAccept, 0, int32(lfd)),
		}
		if _, err := ring.SendTasks([]iouring.SendInfo{initAccept}); err != nil {
			cleanupPartial(db, eventFD, epfd, listenFDs[:r+1], rings[:r+1], bufRings[:r+1])
			return nil, fmt.Errorf("mồi multishot accept ring %d thất bại: %w", r, err)
		}
	}

	sendPool := NewBufferPool(16384, 16384) // 16.384 slots x 16KB = 268MB arena
	batch := NewEventBatch(4096)
	tasks := make([]iouring.ReadInfo, 16384)
	var outTasks [NumRings][]iouring.SendInfo
	for r := 0; r < NumRings; r++ {
		outTasks[r] = make([]iouring.SendInfo, 0, 4096)
	}

	parser := NewParser(bufRings)
	sysMgr := NewSystemManager(db, sendPool, cfg.JWTSecret)

	return &Server{
		cfg:       cfg,
		listenFDs: listenFDs,
		rings:     rings,
		bufRings:  bufRings,
		eventFD:   eventFD,
		epfd:      epfd,
		db:        db,
		sendPool:  sendPool,
		batch:     batch,
		tasks:     tasks,
		outTasks:  outTasks,
		parser:    parser,
		sysMgr:    sysMgr,
	}, nil
}

// Run khởi chạy vòng lặp Proactor Event Loop (Multi-Ring io_uring)
func (s *Server) Run() error {
	runtime.LockOSThread()
	log.Printf("Server đang chạy tại http://%s:%s (Pure Go + 4 io_uring rings)", s.cfg.Host, s.cfg.Port)

	events := make([]syscall.EpollEvent, 1)
	var efdBuf [8]byte

	for {
		s.batch.Reset()

		// 1. Duyệt tuần tự 4 rings: drain completions non-blocking (waitMin = 0)
		for r := uint8(0); r < NumRings; r++ {
			count, err := s.rings[r].RecieveOutputTasks(s.tasks, 0)
			if err != nil {
				if err == syscall.EINTR {
					continue
				}
				return fmt.Errorf("lỗi RecieveOutputTasks ring %d: %w", r, err)
			}

			if count > 0 {
				s.parser.Parse(r, s.tasks[:count], s.batch)
			}
		}

		// 2. Sau khi duyệt 4 rings, nếu có task: gọi sysMgr.Dispatch xử lý DB SQLite, xuất sends tương ứng về ring đã nhận request, rồi submit
		if s.batch.HasEvents() {
			s.sysMgr.Dispatch(s.batch, &s.outTasks)

			for r := 0; r < NumRings; r++ {
				if len(s.outTasks[r]) > 0 {
					_, _ = s.rings[r].SendTasks(s.outTasks[r])
					s.outTasks[r] = s.outTasks[r][:0]
				}
			}
			continue
		}

		// 3. Nếu cả 4 rings đều không có completion nào: gọi submit các ring nếu còn pending
		for r := 0; r < NumRings; r++ {
			if len(s.outTasks[r]) > 0 {
				_, _ = s.rings[r].SendTasks(s.outTasks[r])
				s.outTasks[r] = s.outTasks[r][:0]
			} else {
				_, _ = s.rings[r].Submit()
			}
		}

		// Chờ eventfd qua epoll_wait (timeout 10ms)
		n, err := syscall.EpollWait(s.epfd, events, 10)
		if err != nil && err != syscall.EINTR {
			return fmt.Errorf("lỗi epoll_wait: %w", err)
		}
		if n > 0 {
			// Khi tỉnh dậy, đọc 8 bytes từ eventfd để reset counter rồi tiếp tục vòng lặp
			_, _ = syscall.Read(s.eventFD, efdBuf[:])
		}
	}
}

func cleanupPartial(db *DB, eventFD int, epfd int, listenFDs []int, rings []*iouring.IOUring, bufRings []*iouring.ProvidedBufRing) {
	for _, br := range bufRings {
		if br != nil {
			br.Close()
		}
	}
	for _, r := range rings {
		if r != nil {
			r.Close()
		}
	}
	for _, lfd := range listenFDs {
		if lfd > 0 {
			syscall.Close(lfd)
		}
	}
	if epfd > 0 {
		syscall.Close(epfd)
	}
	if eventFD > 0 {
		syscall.Close(eventFD)
	}
	if db != nil {
		db.Close()
	}
}

func (s *Server) Close() {
	for r := 0; r < NumRings; r++ {
		if s.bufRings[r] != nil {
			s.bufRings[r].Close()
		}
		if s.rings[r] != nil {
			s.rings[r].Close()
		}
		if s.listenFDs[r] > 0 {
			syscall.Close(s.listenFDs[r])
		}
	}
	if s.epfd > 0 {
		syscall.Close(s.epfd)
	}
	if s.eventFD > 0 {
		syscall.Close(s.eventFD)
	}
	if s.db != nil {
		s.db.Close()
	}
}
