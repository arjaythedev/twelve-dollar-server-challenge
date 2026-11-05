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
	cfg      Config
	listenFD int
	ring     *iouring.IOUring
	db       *DB
	bufRing  *iouring.ProvidedBufRing
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

	// 4. Đăng ký Fixed Files Table (65,000 slots)
	if err := iouring.RegisterFileTable(ring.FD(), 65000); err != nil {
		ring.Close()
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("đăng ký fixed files table lỗi: %w", err)
	}

	// 5. Đăng ký Provided Buffer Ring (32,768 buffers x 2048 bytes = 64MB)
	bufRing, err := iouring.NewProvidedBufRing(ring.FD(), 0, 32768, 2048)
	if err != nil {
		ring.Close()
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("đăng ký provided buffer ring lỗi: %w", err)
	}

	sendPool := NewBufferPool(16384, 16384) // 16.384 slots x 16KB = 268MB arena
	batch := NewEventBatch(4096)
	tasks := make([]iouring.ReadInfo, 4096)
	outTasks := make([]iouring.SendInfo, 0, 8192)

	parser := NewParser(bufRing, sendPool, cfg.JWTSecret)
	sysMgr := NewSystemManager(db, sendPool)

	// Mồi 1 lệnh Multishot Accept duy nhất với IORING_FILE_INDEX_ALLOC
	initAccept := iouring.SendInfo{
		Opcode:     iouring.OpAccept,
		FD:         int32(listenFD),
		IOPrio:     iouring.IORING_ACCEPT_MULTISHOT,
		SpliceFDIn: iouring.IORING_FILE_INDEX_ALLOC,
		UserData:   EncodeUserData(EventAccept, 0, int32(listenFD)),
	}
	if _, err := ring.SendTasks([]iouring.SendInfo{initAccept}); err != nil {
		bufRing.Close()
		ring.Close()
		syscall.Close(listenFD)
		db.Close()
		return nil, fmt.Errorf("mồi multishot accept thất bại: %w", err)
	}

	return &Server{
		cfg:      cfg,
		listenFD: listenFD,
		ring:     ring,
		db:       db,
		bufRing:  bufRing,
		sendPool: sendPool,
		batch:    batch,
		tasks:    tasks,
		outTasks: outTasks,
		parser:   parser,
		sysMgr:   sysMgr,
	}, nil
}

// Run khởi chạy vòng lặp Proactor Event Loop (Unified Single-Syscall)
func (s *Server) Run() error {
	runtime.LockOSThread()
	log.Printf("Server đang chạy tại http://%s:%s (Pure Go + io_uring)", s.cfg.Host, s.cfg.Port)

	for {
		// 1. Unified Syscall: Vừa nộp toàn bộ s.outTasks đợt trước, vừa gặt s.tasks cho đợt này!
		count, err := s.ring.SubmitAndRecieve(s.outTasks, s.tasks, 1)
		if err != nil {
			if err == syscall.EINTR {
				continue
			}
			return fmt.Errorf("lỗi SubmitAndRecieve: %w", err)
		}

		s.outTasks = s.outTasks[:0] // Reset slice về 0 phần tử mà không cấp phát lại (Zero-Alloc)

		if count == 0 {
			continue
		}

		// 2. Single-Pass Parse: phân loại completions vào EventBatch sạch
		s.parser.Parse(s.tasks[:count], s.batch, &s.outTasks)

		// 3. Dispatch: 6 Systems xử lý theo chùm
		s.sysMgr.Dispatch(s.batch, &s.outTasks)
	}
}

func (s *Server) Close() {
	if s.bufRing != nil {
		s.bufRing.Close()
	}
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
