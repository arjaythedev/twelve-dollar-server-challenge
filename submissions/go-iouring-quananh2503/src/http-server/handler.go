package httpserver

import (
	"time"

	iouring "go-iouring-quananh2503/src/io-uring"
	"go-iouring-quananh2503/src/sqlite"
)

// =============================================================================
// HELPER ĐÓNG GÓI HTTP RESPONSE (ZERO ALLOCATION)
// =============================================================================

func buildHTTPResponse(w *PacketWriter, statusText string, body []byte) {
	w.Reset()
	w.WriteString("HTTP/1.1 ")
	w.WriteString(statusText)
	w.WriteString("\r\nContent-Type: application/json\r\nContent-Length: ")
	w.WriteInt(len(body))
	w.WriteString("\r\nConnection: keep-alive\r\n\r\n")
	w.WriteBytes(body)
}

func appendResponse(outTasks *[]iouring.SendInfo, sendPool *BufferPool, fixedFD int32, data []byte) {
	bufID, err := sendPool.Acquire()
	if err != nil {
		return
	}
	buf := sendPool.Get(bufID)
	n := copy(buf, data)

	*outTasks = append(*outTasks, iouring.SendInfo{
		Opcode:   iouring.OpSend,
		Flags:    iouring.IOSQE_FIXED_FILE,
		FD:       fixedFD,
		Buf:      buf[:n],
		UserData: EncodeUserData(EventSend, bufID, fixedFD),
	})
}

// =============================================================================
// 1. ACCEPT SYSTEM (TREO MULTISHOT RECV TRÊN FIXED FD MỚI)
// =============================================================================

type AcceptSystem struct{}

func NewAcceptSystem() *AcceptSystem {
	return &AcceptSystem{}
}

func (s *AcceptSystem) Process(events []AcceptEvent, outTasks *[]iouring.SendInfo) {
	for i := range events {
		ev := &events[i]
		if ev.ClientFD < 0 {
			continue
		}

		// Treo 1 lệnh Multishot Recv duy nhất trên client fixed FD mới
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:   iouring.OpRecv,
			Flags:    iouring.IOSQE_FIXED_FILE | iouring.IOSQE_BUFFER_SELECT,
			IOPrio:   iouring.IORING_RECV_MULTISHOT,
			FD:       ev.ClientFD,
			BufIndex: 0,
			UserData: EncodeUserData(EventRecv, 0, ev.ClientFD),
		})
	}
}

// =============================================================================
// 2. HEALTH SYSTEM (BATCH & COALESCING)
// =============================================================================

type HealthSystem struct {
	db       *DB
	writer   *PacketWriter
	bodyW    *PacketWriter
	SendPool *BufferPool
}

func NewHealthSystem(db *DB, sendPool *BufferPool) *HealthSystem {
	return &HealthSystem{
		db:       db,
		writer:   NewPacketWriter(1024),
		bodyW:    NewPacketWriter(512),
		SendPool: sendPool,
	}
}

func (s *HealthSystem) Process(events []HealthEvent, outTasks *[]iouring.SendInfo) {
	if len(events) == 0 {
		return
	}

	now := time.Now().Unix()
	uptime := int(now - s.db.StartTime)

	// 1 lần kiểm tra C duy nhất cho toàn bộ batch
	ok := sqlite.DBHealth()

	s.bodyW.Reset()
	if !ok {
		s.bodyW.WriteString(`{"status":"degraded","db":"unreachable"}`)
		buildHTTPResponse(s.writer, "503 Service Unavailable", s.bodyW.Bytes())
	} else {
		s.bodyW.WriteString(`{"status":"ok","db":"ok","uptime_s":`)
		s.bodyW.WriteInt(uptime)
		s.bodyW.WriteByte('}')
		buildHTTPResponse(s.writer, "200 OK", s.bodyW.Bytes())
	}

	respBytes := s.writer.Bytes()

	for i := range events {
		ev := &events[i]
		appendResponse(outTasks, s.SendPool, ev.FD, respBytes)
	}
}

// =============================================================================
// 3. FEED SYSTEM (DIRECT B-TREE READ COALESCING)
// =============================================================================

type FeedSystem struct {
	db       *DB
	writer   *PacketWriter
	bodyW    *PacketWriter
	SendPool *BufferPool
	feedBuf  []byte
}

func NewFeedSystem(db *DB, sendPool *BufferPool) *FeedSystem {
	return &FeedSystem{
		db:       db,
		writer:   NewPacketWriter(16384),
		bodyW:    NewPacketWriter(16384),
		SendPool: sendPool,
		feedBuf:  make([]byte, 65536),
	}
}

func (s *FeedSystem) Process(events []FeedEvent, outTasks *[]iouring.SendInfo) {
	if len(events) == 0 {
		return
	}

	// 1 lần đọc C duy nhất vào buffer pre-allocated cho toàn bộ batch FeedEvents (Zero Allocation)
	n, err := sqlite.GetFeedInto(s.feedBuf)
	if err == nil && n > 0 {
		buildHTTPResponse(s.writer, "200 OK", s.feedBuf[:n])
	} else {
		s.bodyW.Reset()
		s.bodyW.WriteString(`{"error":"internal server error"}`)
		buildHTTPResponse(s.writer, "500 Internal Server Error", s.bodyW.Bytes())
	}

	respBytes := s.writer.Bytes()

	for i := range events {
		ev := &events[i]
		appendResponse(outTasks, s.SendPool, ev.FD, respBytes)
	}
}

// =============================================================================
// 4. GET POST SYSTEM (SORT & DEDUPLICATED B-TREE BATCH READ)
// =============================================================================

type GetPostSystem struct {
	db       *DB
	writer   *PacketWriter
	bodyW    *PacketWriter
	SendPool *BufferPool

	ids    []int64
	items  []sqlite.PostBatchItem
	outBuf []byte
}

func NewGetPostSystem(db *DB, sendPool *BufferPool) *GetPostSystem {
	return &GetPostSystem{
		db:       db,
		writer:   NewPacketWriter(4096),
		bodyW:    NewPacketWriter(2048),
		SendPool: sendPool,
		ids:      make([]int64, 4096),
		items:    make([]sqlite.PostBatchItem, 4096),
		outBuf:   make([]byte, 2*1024*1024),
	}
}

func (s *GetPostSystem) Process(events []GetPostEvent, outTasks *[]iouring.SendInfo) {
	n := len(events)
	if n == 0 {
		return
	}

	if n > len(s.ids) {
		s.ids = make([]int64, n)
		s.items = make([]sqlite.PostBatchItem, n)
	}
	if n*1024 > len(s.outBuf) {
		s.outBuf = make([]byte, n*1024)
	}

	for i := 0; i < n; i++ {
		s.ids[i] = int64(events[i].PostID)
	}

	// 1 CGO call duy nhất & 1 transaction SQLite duy nhất cho cả batch!
	err := sqlite.GetPostsBatch(s.ids[:n], s.outBuf, s.items[:n])

	for i := 0; i < n; i++ {
		ev := &events[i]

		if err != nil {
			s.bodyW.Reset()
			s.bodyW.WriteString(`{"error":"internal server error"}`)
			buildHTTPResponse(s.writer, "500 Internal Server Error", s.bodyW.Bytes())
		} else {
			item := &s.items[i]
			if item.Status == 200 {
				body := s.outBuf[item.Offset : item.Offset+item.Len]
				buildHTTPResponse(s.writer, "200 OK", body)
			} else if item.Status == 404 {
				s.bodyW.Reset()
				s.bodyW.WriteString(`{"error":"post not found"}`)
				buildHTTPResponse(s.writer, "404 Not Found", s.bodyW.Bytes())
			} else {
				s.bodyW.Reset()
				s.bodyW.WriteString(`{"error":"internal server error"}`)
				buildHTTPResponse(s.writer, "500 Internal Server Error", s.bodyW.Bytes())
			}
		}

		appendResponse(outTasks, s.SendPool, ev.FD, s.writer.Bytes())
	}
}

// =============================================================================
// 5. CREATE POST SYSTEM (GROUP COMMIT BATCH WRITE IN C)
// =============================================================================

type CreatePostSystem struct {
	db       *DB
	writer   *PacketWriter
	bodyW    *PacketWriter
	SendPool *BufferPool

	userIDs     []int64
	bodyBuf     []byte
	bodyOffsets []int32
	bodyLens    []int32
	postIDs     []int64
	statuses    []int32
	nowBuf      [32]byte
}

func NewCreatePostSystem(db *DB, sendPool *BufferPool) *CreatePostSystem {
	return &CreatePostSystem{
		db:          db,
		writer:      NewPacketWriter(4096),
		bodyW:       NewPacketWriter(2048),
		SendPool:    sendPool,
		userIDs:     make([]int64, 4096),
		bodyBuf:     make([]byte, 0, 1024*1024),
		bodyOffsets: make([]int32, 4096),
		bodyLens:    make([]int32, 4096),
		postIDs:     make([]int64, 4096),
		statuses:    make([]int32, 4096),
	}
}

func (s *CreatePostSystem) Process(events []CreatePostEvent, outTasks *[]iouring.SendInfo) {
	n := len(events)
	if n == 0 {
		return
	}

	if n > len(s.userIDs) {
		s.userIDs = make([]int64, n)
		s.bodyOffsets = make([]int32, n)
		s.bodyLens = make([]int32, n)
		s.postIDs = make([]int64, n)
		s.statuses = make([]int32, n)
	}

	s.bodyBuf = s.bodyBuf[:0]

	for i := 0; i < n; i++ {
		ev := &events[i]
		s.userIDs[i] = int64(ev.UserID)
		s.bodyOffsets[i] = int32(len(s.bodyBuf))
		s.bodyLens[i] = int32(len(ev.Body))
		s.bodyBuf = append(s.bodyBuf, ev.Body...)
	}

	// Go tính timestamp UTC đúng 1 lần (Zero-Alloc), truyền xuống C và dùng luôn cho JSON response
	now := time.Now().UTC()
	nowFormatted := now.AppendFormat(s.nowBuf[:0], "2006-01-02T15:04:05.000Z")

	// 1 CGO call & 1 Write Transaction duy nhất (Group Commit) cho cả batch!
	err := sqlite.CreatePostsBatch(
		s.userIDs[:n],
		s.bodyBuf,
		s.bodyOffsets[:n],
		s.bodyLens[:n],
		nowFormatted,
		s.postIDs[:n],
		s.statuses[:n],
	)

	for i := 0; i < n; i++ {
		ev := &events[i]

		if err != nil || s.statuses[i] != 201 {
			s.bodyW.Reset()
			s.bodyW.WriteString(`{"error":"internal server error"}`)
			buildHTTPResponse(s.writer, "500 Internal Server Error", s.bodyW.Bytes())
		} else {
			s.bodyW.Reset()
			s.bodyW.WriteString(`{"post":{"id":`)
			s.bodyW.WriteInt(int(s.postIDs[i]))
			s.bodyW.WriteString(`,"body":"`)
			s.bodyW.WriteBytes(ev.Body)
			s.bodyW.WriteString(`","created_at":"`)
			s.bodyW.WriteBytes(nowFormatted)
			s.bodyW.WriteString(`","author":"`)
			s.bodyW.WriteBytes(ev.Username)
			s.bodyW.WriteString(`","like_count":0}}`)
			buildHTTPResponse(s.writer, "201 Created", s.bodyW.Bytes())
		}

		appendResponse(outTasks, s.SendPool, ev.FD, s.writer.Bytes())
	}
}

// =============================================================================
// 6. LIKE POST SYSTEM (SORT & DEDUPLICATED GROUP COMMIT IN C)
// =============================================================================

type LikePostSystem struct {
	db       *DB
	writer   *PacketWriter
	bodyW    *PacketWriter
	SendPool *BufferPool

	userIDs []int64
	postIDs []int64
	results []int32
	nowBuf  [32]byte
}

func NewLikePostSystem(db *DB, sendPool *BufferPool) *LikePostSystem {
	return &LikePostSystem{
		db:       db,
		writer:   NewPacketWriter(1024),
		bodyW:    NewPacketWriter(512),
		SendPool: sendPool,
		userIDs:  make([]int64, 4096),
		postIDs:  make([]int64, 4096),
		results:  make([]int32, 4096),
	}
}

func (s *LikePostSystem) Process(events []LikePostEvent, outTasks *[]iouring.SendInfo) {
	n := len(events)
	if n == 0 {
		return
	}

	if n > len(s.userIDs) {
		s.userIDs = make([]int64, n)
		s.postIDs = make([]int64, n)
		s.results = make([]int32, n)
	}

	for i := 0; i < n; i++ {
		s.userIDs[i] = int64(events[i].UserID)
		s.postIDs[i] = int64(events[i].PostID)
	}

	// Go tính timestamp UTC đúng 1 lần (Zero-Alloc), truyền xuống C loại bỏ hàm strftime của SQLite
	now := time.Now().UTC()
	nowFormatted := now.AppendFormat(s.nowBuf[:0], "2006-01-02T15:04:05.000Z")

	// 1 CGO call duy nhất: qsort, B-tree cache lookup, in-batch deduplication, Group Commit
	err := sqlite.InsertLikesBatch(s.userIDs[:n], s.postIDs[:n], nowFormatted, s.results[:n])

	for i := 0; i < n; i++ {
		ev := &events[i]

		if err != nil {
			s.bodyW.Reset()
			s.bodyW.WriteString(`{"error":"internal server error"}`)
			buildHTTPResponse(s.writer, "500 Internal Server Error", s.bodyW.Bytes())
		} else {
			res := s.results[i]
			if res == 404 {
				// SPEC: 404 {"error":"post not found"}
				s.bodyW.Reset()
				s.bodyW.WriteString(`{"error":"post not found"}`)
				buildHTTPResponse(s.writer, "404 Not Found", s.bodyW.Bytes())
			} else if res == 201 {
				// Like lần đầu: 201 Created
				s.bodyW.Reset()
				s.bodyW.WriteString(`{"liked":true,"already_liked":false,"post_id":`)
				s.bodyW.WriteInt(ev.PostID)
				s.bodyW.WriteByte('}')
				buildHTTPResponse(s.writer, "201 Created", s.bodyW.Bytes())
			} else if res == 200 {
				// Like lặp lại: 200 OK
				s.bodyW.Reset()
				s.bodyW.WriteString(`{"liked":true,"already_liked":true,"post_id":`)
				s.bodyW.WriteInt(ev.PostID)
				s.bodyW.WriteByte('}')
				buildHTTPResponse(s.writer, "200 OK", s.bodyW.Bytes())
			} else {
				s.bodyW.Reset()
				s.bodyW.WriteString(`{"error":"internal server error"}`)
				buildHTTPResponse(s.writer, "500 Internal Server Error", s.bodyW.Bytes())
			}
		}

		appendResponse(outTasks, s.SendPool, ev.FD, s.writer.Bytes())
	}
}

// =============================================================================
// 7. SYSTEM MANAGER (ĐIỀU PHỐI 6 SYSTEMS)
// =============================================================================

type SystemManager struct {
	AcceptSys     *AcceptSystem
	HealthSys     *HealthSystem
	FeedSys       *FeedSystem
	GetPostSys    *GetPostSystem
	CreatePostSys *CreatePostSystem
	LikePostSys   *LikePostSystem
}

func NewSystemManager(db *DB, sendPool *BufferPool) *SystemManager {
	return &SystemManager{
		AcceptSys:     NewAcceptSystem(),
		HealthSys:     NewHealthSystem(db, sendPool),
		FeedSys:       NewFeedSystem(db, sendPool),
		GetPostSys:    NewGetPostSystem(db, sendPool),
		CreatePostSys: NewCreatePostSystem(db, sendPool),
		LikePostSys:   NewLikePostSystem(db, sendPool),
	}
}

// Dispatch thực thi tuần tự 6 Systems trên mảng EventBatch sạch
func (m *SystemManager) Dispatch(batch *EventBatch, outTasks *[]iouring.SendInfo) {
	if len(batch.AcceptEvents) > 0 {
		m.AcceptSys.Process(batch.AcceptEvents, outTasks)
	}
	if len(batch.HealthEvents) > 0 {
		m.HealthSys.Process(batch.HealthEvents, outTasks)
	}
	if len(batch.FeedEvents) > 0 {
		m.FeedSys.Process(batch.FeedEvents, outTasks)
	}
	if len(batch.GetPostEvents) > 0 {
		m.GetPostSys.Process(batch.GetPostEvents, outTasks)
	}
	if len(batch.CreatePostEvents) > 0 {
		m.CreatePostSys.Process(batch.CreatePostEvents, outTasks)
	}
	if len(batch.LikePostEvents) > 0 {
		m.LikePostSys.Process(batch.LikePostEvents, outTasks)
	}
}
