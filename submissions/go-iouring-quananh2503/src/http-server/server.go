package httpserver

import (
	"fmt"
	"log"
	"net"
	"strconv"
	"syscall"

	iouring "go-iouring-quananh2503/src/io-uring"
)

type Server struct {
	cfg      Config
	listenFD int
	ring     *iouring.IOUring
	db       *DB
	recvPool *BufferPool
	sendPool *BufferPool
	batch    *EventBatch
	tasks    []iouring.ReadInfo
	outTasks []iouring.SendInfo
	parser   *Parser
	sysMgr   *SystemManager
}

func NewServer(cfg Config) (*Server, error) {
	// 1. Khởi tạo DB SQLite
	db, err := OpenDB(cfg.DBPath)
	if err != nil {
		return nil, fmt.Errorf("khởi tạo DB lỗi: %w", err)
	}

	// 2. Khởi tạo Socket lắng nghe TCP
	listenFD, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
	if err != nil {
		db.Close()
		return nil, fmt.Errorf("tạo socket lỗi: %w", err)
	}

	if err := syscall.SetsockoptInt(listenFD, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); err != nil {
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("setsockopt SO_REUSEADDR lỗi: %w", err)
	}

	_ = syscall.SetsockoptInt(listenFD, syscall.SOL_SOCKET, 0xf, 1) // SO_REUSEPORT (0xf trên Linux)

	// BẮT BUỘC: Đặt listenFD thành Non-blocking để accept4 không bao giờ block tiến trình
	if err := syscall.SetNonblock(listenFD, true); err != nil {
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("set nonblock listenFD lỗi: %w", err)
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

	if err := syscall.Bind(listenFD, sa); err != nil {
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("bind socket %s:%s lỗi: %w", cfg.Host, cfg.Port, err)
	}

	if err := syscall.Listen(listenFD, 4096); err != nil {
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("listen socket lỗi: %w", err)
	}

	// 3. Khởi tạo Ring io_uring (dung lượng 4096 entries)
	ring, err := iouring.NewRing(4096)
	if err != nil {
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("khởi tạo io_uring ring lỗi: %w", err)
	}

	// 4. Khởi tạo các bộ đệm và System đáp ứng trọn vẹn 20.000 VUs
	recvPool := NewBufferPool(40000, 2048)  // 40.000 slots x 2KB = 80MB arena
	sendPool := NewBufferPool(16384, 16384) // 16.384 slots x 16KB = 268MB arena
	batch := NewEventBatch(4096)
	tasks := make([]iouring.ReadInfo, 4096)
	outTasks := make([]iouring.SendInfo, 0, 8192)

	parser := NewParser(recvPool, sendPool, cfg.JWTSecret)
	sysMgr := NewSystemManager(db, sendPool, recvPool)

	// Mồi sẵn 64 lệnh Accept đồng thời vào ring để đón lượng kết nối ồ ạt
	initAccepts := make([]iouring.SendInfo, 64)
	for i := 0; i < 64; i++ {
		initAccepts[i] = iouring.SendInfo{
			Opcode:   iouring.OpAccept,
			FD:       int32(listenFD),
			UserData: EncodeUserData(EventAccept, 0, int32(listenFD)),
		}
	}
	if _, err := ring.SendTasks(initAccepts); err != nil {
		ring.Close()
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("mồi 64 accepts thất bại: %w", err)
	}

	return &Server{
		cfg:      cfg,
		listenFD: listenFD,
		ring:     ring,
		db:       db,
		recvPool: recvPool,
		sendPool: sendPool,
		batch:    batch,
		tasks:    tasks,
		outTasks: outTasks,
		parser:   parser,
		sysMgr:   sysMgr,
	}, nil
}

// Run khởi chạy vòng lặp Proactor Event Loop
func (s *Server) Run() error {
	log.Printf("Server đang chạy tại http://%s:%s (Pure Go + io_uring)", s.cfg.Host, s.cfg.Port)

	for {
		// 1. Chờ ít nhất 1 kết quả hoàn tất từ CQ ring (waitMin = 1)
		count, err := s.ring.RecieveOutputTasks(s.tasks, 1)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return fmt.Errorf("lỗi RecieveOutputTasks: %w", err)
		}

		if count == 0 {
			continue
		}

		s.outTasks = s.outTasks[:0] // Reset slice về 0 phần tử mà không cấp phát lại (Zero-Alloc)

		// 2. Single-Pass Parse: phân loại completions vào EventBatch sạch
		s.parser.Parse(s.tasks[:count], s.batch, &s.outTasks)

		// 3. Dispatch: 6 Systems xử lý theo chùm
		s.sysMgr.Dispatch(s.batch, &s.outTasks)

		// 4. Batch Submit: Nộp toàn bộ các task tích lũy một lần duy nhất ở cuối vòng lặp
		if len(s.outTasks) > 0 {
			if _, err := s.ring.SendTasks(s.outTasks); err != nil {
				log.Printf("Lỗi SendTasks batch cuối loop: %v", err)
			}
		}
	}
}

func (s *Server) Close() {
	if s.ring != nil {
		s.ring.Close()
	}
	if s.listenFD > 0 {
		syscall.Close(s.listenFD)
	}
	if s.db != nil {
		s.db.Close()
	}
}
