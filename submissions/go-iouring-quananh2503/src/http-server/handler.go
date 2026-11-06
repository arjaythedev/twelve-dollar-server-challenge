package httpserver

import (
	"time"

	iouring "go-iouring-quananh2503/src/io-uring"
	"go-iouring-quananh2503/src/sqlite"
)

// =============================================================================
// 1. CENTRALIZED RESPONSE WRITER (GOM TOÀN BỘ RESPONSE FORMATTING & SENDPOOL)
// =============================================================================

type ResponseWriter struct {
	SendPool *BufferPool
	writer   *PacketWriter
	bodyW    *PacketWriter
}

func NewResponseWriter(sendPool *BufferPool) *ResponseWriter {
	return &ResponseWriter{
		SendPool: sendPool,
		writer:   NewPacketWriter(16384),
		bodyW:    NewPacketWriter(16384),
	}
}

func (rw *ResponseWriter) appendSend(fixedFD int32, data []byte, outTasks *[]iouring.SendInfo) {
	bufID, err := rw.SendPool.Acquire()
	if err != nil {
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:     iouring.OpClose,
			Flags:      iouring.IOSQE_CQE_SKIP_SUCCESS,
			SpliceFDIn: fixedFD + 1,
			UserData:   EncodeUserData(EventClose, 0, fixedFD),
		})
		return
	}
	buf := rw.SendPool.Get(bufID)
	n := copy(buf, data)

	*outTasks = append(*outTasks, iouring.SendInfo{
		Opcode:   iouring.OpSend,
		Flags:    iouring.IOSQE_FIXED_FILE | iouring.IOSQE_CQE_SKIP_SUCCESS,
		FD:       fixedFD,
		Buf:      buf[:n],
		UserData: EncodeUserData(EventSend, bufID, fixedFD),
	})
}

func (rw *ResponseWriter) buildHTTPResponse(statusText string, body []byte) {
	rw.writer.Reset()
	rw.writer.WriteString("HTTP/1.1 ")
	rw.writer.WriteString(statusText)
	rw.writer.WriteString("\r\nContent-Type: application/json\r\nContent-Length: ")
	rw.writer.WriteInt(len(body))
	rw.writer.WriteString("\r\nConnection: keep-alive\r\n\r\n")
	rw.writer.WriteBytes(body)
}

func (rw *ResponseWriter) WriteErrors(tasks []ErrorTask, outTasks *[]iouring.SendInfo) {
	for i := range tasks {
		task := &tasks[i]
		statusText := "400 Bad Request"
		switch task.Status {
		case 401:
			statusText = "401 Unauthorized"
		case 404:
			statusText = "404 Not Found"
		case 500:
			statusText = "500 Internal Server Error"
		case 503:
			statusText = "503 Service Unavailable"
		}

		rw.bodyW.Reset()
		rw.bodyW.WriteString(`{"error":"`)
		rw.bodyW.WriteString(task.Msg)
		rw.bodyW.WriteString(`"}`)

		rw.buildHTTPResponse(statusText, rw.bodyW.Bytes())
		rw.appendSend(task.FD, rw.writer.Bytes(), outTasks)
	}
}

func (rw *ResponseWriter) appendSendShared(fds []int32, data []byte, outTasks *[]iouring.SendInfo) {
	if len(fds) == 0 {
		return
	}
	bufID, err := rw.SendPool.Acquire()
	if err != nil {
		return
	}
	buf := rw.SendPool.Get(bufID)
	n := copy(buf, data)
	sharedBuf := buf[:n]

	for _, fd := range fds {
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:   iouring.OpSend,
			Flags:    iouring.IOSQE_FIXED_FILE | iouring.IOSQE_CQE_SKIP_SUCCESS,
			FD:       fd,
			Buf:      sharedBuf,
			UserData: EncodeUserData(EventSend, bufID, fd),
		})
	}
}

func (rw *ResponseWriter) WriteHealth(tasks []int32, ok bool, uptime int, outTasks *[]iouring.SendInfo) {
	rw.bodyW.Reset()
	if !ok {
		rw.bodyW.WriteString(`{"status":"degraded","db":"unreachable"}`)
		rw.buildHTTPResponse("503 Service Unavailable", rw.bodyW.Bytes())
	} else {
		rw.bodyW.WriteString(`{"status":"ok","db":"ok","uptime_s":`)
		rw.bodyW.WriteInt(uptime)
		rw.bodyW.WriteByte('}')
		rw.buildHTTPResponse("200 OK", rw.bodyW.Bytes())
	}

	rw.appendSendShared(tasks, rw.writer.Bytes(), outTasks)
}

func (rw *ResponseWriter) WriteFeed(tasks []int32, feedBytes []byte, err error, outTasks *[]iouring.SendInfo) {
	if err == nil && len(feedBytes) > 0 {
		rw.buildHTTPResponse("200 OK", feedBytes)
	} else {
		rw.bodyW.Reset()
		rw.bodyW.WriteString(`{"error":"internal server error"}`)
		rw.buildHTTPResponse("500 Internal Server Error", rw.bodyW.Bytes())
	}

	rw.appendSendShared(tasks, rw.writer.Bytes(), outTasks)
}

func (rw *ResponseWriter) WriteGetPosts(tasks []GetPostTask, items []sqlite.PostBatchItem, outBuf []byte, err error, outTasks *[]iouring.SendInfo) {
	for i := range tasks {
		task := &tasks[i]
		if err != nil {
			rw.bodyW.Reset()
			rw.bodyW.WriteString(`{"error":"internal server error"}`)
			rw.buildHTTPResponse("500 Internal Server Error", rw.bodyW.Bytes())
		} else {
			item := &items[i]
			if item.Status == 200 {
				body := outBuf[item.Offset : item.Offset+item.Len]
				rw.buildHTTPResponse("200 OK", body)
			} else if item.Status == 404 {
				rw.bodyW.Reset()
				rw.bodyW.WriteString(`{"error":"post not found"}`)
				rw.buildHTTPResponse("404 Not Found", rw.bodyW.Bytes())
			} else {
				rw.bodyW.Reset()
				rw.bodyW.WriteString(`{"error":"internal server error"}`)
				rw.buildHTTPResponse("500 Internal Server Error", rw.bodyW.Bytes())
			}
		}
		rw.appendSend(task.FD, rw.writer.Bytes(), outTasks)
	}
}

func (rw *ResponseWriter) WriteCreatePosts(tasks []CreatePostTask, statuses []int32, nowFormatted []byte, err error, outTasks *[]iouring.SendInfo) {
	for i := range tasks {
		task := &tasks[i]
		if err != nil || statuses[i] != 201 {
			rw.bodyW.Reset()
			rw.bodyW.WriteString(`{"error":"internal server error"}`)
			rw.buildHTTPResponse("500 Internal Server Error", rw.bodyW.Bytes())
		} else {
			rw.bodyW.Reset()
			rw.bodyW.WriteString(`{"post":{"id":`)
			rw.bodyW.WriteInt(int(task.PostID))
			rw.bodyW.WriteString(`,"body":"`)
			rw.bodyW.WriteBytes(task.Body)
			rw.bodyW.WriteString(`","created_at":"`)
			rw.bodyW.WriteBytes(nowFormatted)
			rw.bodyW.WriteString(`","author":"`)
			rw.bodyW.WriteBytes(task.Username)
			rw.bodyW.WriteString(`","like_count":0}}`)
			rw.buildHTTPResponse("201 Created", rw.bodyW.Bytes())
		}
		rw.appendSend(task.FD, rw.writer.Bytes(), outTasks)
	}
}

func (rw *ResponseWriter) WriteLikePosts(tasks []LikePostTask, results []int32, err error, outTasks *[]iouring.SendInfo) {
	for i := range tasks {
		task := &tasks[i]
		if err != nil {
			rw.bodyW.Reset()
			rw.bodyW.WriteString(`{"error":"internal server error"}`)
			rw.buildHTTPResponse("500 Internal Server Error", rw.bodyW.Bytes())
		} else {
			res := results[i]
			if res == 404 {
				rw.bodyW.Reset()
				rw.bodyW.WriteString(`{"error":"post not found"}`)
				rw.buildHTTPResponse("404 Not Found", rw.bodyW.Bytes())
			} else if res == 201 {
				rw.bodyW.Reset()
				rw.bodyW.WriteString(`{"liked":true,"already_liked":false,"post_id":`)
				rw.bodyW.WriteInt(int(task.PostID))
				rw.bodyW.WriteByte('}')
				rw.buildHTTPResponse("201 Created", rw.bodyW.Bytes())
			} else if res == 200 {
				rw.bodyW.Reset()
				rw.bodyW.WriteString(`{"liked":true,"already_liked":true,"post_id":`)
				rw.bodyW.WriteInt(int(task.PostID))
				rw.bodyW.WriteByte('}')
				rw.buildHTTPResponse("200 OK", rw.bodyW.Bytes())
			} else {
				rw.bodyW.Reset()
				rw.bodyW.WriteString(`{"error":"internal server error"}`)
				rw.buildHTTPResponse("500 Internal Server Error", rw.bodyW.Bytes())
			}
		}
		rw.appendSend(task.FD, rw.writer.Bytes(), outTasks)
	}
}

// =============================================================================
// 2. CONNECTION SYSTEM (LIFECYCLE: ACCEPT, REARM, CLOSE)
// =============================================================================

type ConnectionSystem struct{}

func NewConnectionSystem() *ConnectionSystem {
	return &ConnectionSystem{}
}

func (s *ConnectionSystem) ProcessLifecycle(batch *EventBatch, outTasks *[]iouring.SendInfo) {
	// 1. Accept events: Treo 1 lệnh Multishot Recv duy nhất trên client fixed FD mới
	for i := range batch.AcceptEvents {
		ev := &batch.AcceptEvents[i]
		if ev.ClientFD < 0 {
			continue
		}
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:   iouring.OpRecv,
			Flags:    iouring.IOSQE_FIXED_FILE | iouring.IOSQE_BUFFER_SELECT,
			IOPrio:   iouring.IORING_RECV_MULTISHOT,
			FD:       ev.ClientFD,
			BufIndex: 0,
			UserData: EncodeUserData(EventRecv, 0, ev.ClientFD),
		})
	}

	// 2. Rearm Accept multishot
	for _, fd := range batch.RearmAcceptQueue {
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:     iouring.OpAccept,
			FD:         fd,
			IOPrio:     iouring.IORING_ACCEPT_MULTISHOT,
			SpliceFDIn: iouring.IORING_FILE_INDEX_ALLOC,
			UserData:   EncodeUserData(EventAccept, 0, fd),
		})
	}

	// 3. Rearm Recv multishot
	for _, fd := range batch.RearmRecvQueue {
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:   iouring.OpRecv,
			Flags:    iouring.IOSQE_FIXED_FILE | iouring.IOSQE_BUFFER_SELECT,
			IOPrio:   iouring.IORING_RECV_MULTISHOT,
			FD:       fd,
			BufIndex: 0,
			UserData: EncodeUserData(EventRecv, 0, fd),
		})
	}

	// 4. Close sockets
	for _, fd := range batch.CloseQueue {
		*outTasks = append(*outTasks, iouring.SendInfo{
			Opcode:     iouring.OpClose,
			Flags:      iouring.IOSQE_CQE_SKIP_SUCCESS,
			SpliceFDIn: fd + 1,
			UserData:   EncodeUserData(EventClose, 0, fd),
		})
	}
}

// =============================================================================
// 3. HEALTH SYSTEM (BATCH & COALESCING)
// =============================================================================

type HealthSystem struct {
	db         *DB
	respWriter *ResponseWriter
}

func NewHealthSystem(db *DB, respWriter *ResponseWriter) *HealthSystem {
	return &HealthSystem{
		db:         db,
		respWriter: respWriter,
	}
}

func (s *HealthSystem) Process(tasks []int32, outTasks *[]iouring.SendInfo) {
	if len(tasks) == 0 {
		return
	}
	now := time.Now().Unix()
	uptime := int(now - s.db.StartTime)
	ok := sqlite.DBHealth()
	s.respWriter.WriteHealth(tasks, ok, uptime, outTasks)
}

// =============================================================================
// 4. FEED SYSTEM (DIRECT B-TREE READ COALESCING)
// =============================================================================

type FeedSystem struct {
	db         *DB
	respWriter *ResponseWriter
	feedBuf    []byte
}

func NewFeedSystem(db *DB, respWriter *ResponseWriter) *FeedSystem {
	return &FeedSystem{
		db:         db,
		respWriter: respWriter,
		feedBuf:    make([]byte, 65536),
	}
}

func (s *FeedSystem) Process(tasks []int32, outTasks *[]iouring.SendInfo) {
	if len(tasks) == 0 {
		return
	}
	n, err := sqlite.GetFeedInto(s.feedBuf)
	var feedBytes []byte
	if err == nil && n > 0 {
		feedBytes = s.feedBuf[:n]
	}
	s.respWriter.WriteFeed(tasks, feedBytes, err, outTasks)
}

// =============================================================================
// 5. GET POST SYSTEM (SORT & DEDUPLICATED B-TREE BATCH READ)
// =============================================================================

type GetPostSystem struct {
	db         *DB
	respWriter *ResponseWriter
	ids        []int64
	items      []sqlite.PostBatchItem
	outBuf     []byte
}

func NewGetPostSystem(db *DB, respWriter *ResponseWriter) *GetPostSystem {
	return &GetPostSystem{
		db:         db,
		respWriter: respWriter,
		ids:        make([]int64, 4096),
		items:      make([]sqlite.PostBatchItem, 4096),
		outBuf:     make([]byte, 2*1024*1024),
	}
}

func (s *GetPostSystem) Process(batch *EventBatch, outTasks *[]iouring.SendInfo) {
	tasks := batch.GetPostTasks
	n := len(tasks)
	if n == 0 {
		return
	}

	// Two-pointer in-place filtering
	validCount := 0
	for i := range tasks {
		task := &tasks[i]
		if task.PostID <= 0 {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: 400,
				Msg:    "invalid post id",
			})
			continue
		}
		tasks[validCount] = *task
		validCount++
	}
	tasks = tasks[:validCount]
	batch.GetPostTasks = tasks
	if validCount == 0 {
		return
	}

	if validCount > len(s.ids) {
		s.ids = make([]int64, validCount)
		s.items = make([]sqlite.PostBatchItem, validCount)
	}
	if validCount*1024 > len(s.outBuf) {
		s.outBuf = make([]byte, validCount*1024)
	}

	for i := 0; i < validCount; i++ {
		s.ids[i] = tasks[i].PostID
	}

	err := sqlite.GetPostsBatch(s.ids[:validCount], s.outBuf, s.items[:validCount])
	s.respWriter.WriteGetPosts(tasks, s.items[:validCount], s.outBuf, err, outTasks)
}

// =============================================================================
// 6. CREATE POST SYSTEM (TWO-POINTER IN-PLACE FILTERING & GROUP COMMIT CGO)
// =============================================================================

type CreatePostSystem struct {
	db          *DB
	verifier    *JWTVerifier
	respWriter  *ResponseWriter
	userIDs     []int64
	bodyBuf     []byte
	usernameBuf []byte
	bodyOffsets []int32
	bodyLens    []int32
	postIDs     []int64
	statuses    []int32
	nowBuf      [32]byte
}

func NewCreatePostSystem(db *DB, respWriter *ResponseWriter, jwtSecret []byte) *CreatePostSystem {
	return &CreatePostSystem{
		db:          db,
		verifier:    NewJWTVerifier(jwtSecret),
		respWriter:  respWriter,
		userIDs:     make([]int64, 4096),
		bodyBuf:     make([]byte, 0, 1024*1024),
		usernameBuf: make([]byte, 0, 65536),
		bodyOffsets: make([]int32, 4096),
		bodyLens:    make([]int32, 4096),
		postIDs:     make([]int64, 4096),
		statuses:    make([]int32, 4096),
	}
}

func (s *CreatePostSystem) Process(batch *EventBatch, outTasks *[]iouring.SendInfo) {
	tasks := batch.CreatePostTasks
	if len(tasks) == 0 {
		return
	}

	now := time.Now().Unix()
	validCount := 0
	s.bodyBuf = s.bodyBuf[:0]
	s.usernameBuf = s.usernameBuf[:0]

	// Two-Pointer In-Place Filtering & Auth
	for i := range tasks {
		task := &tasks[i]

		// 1. Kiểm tra Auth TRƯỚC theo đúng SPEC
		if len(task.Token) == 0 {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: 401,
				Msg:    "missing bearer token",
			})
			continue
		}

		userID, username, authErr, status := s.verifier.Verify(task.Token, now)
		if authErr != "" {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: status,
				Msg:    authErr,
			})
			continue
		}

		// 2. Kiểm tra JSON Body SAU khi Auth hợp lệ
		body, bodyErr := extractJSONBodyField(task.Body)
		if bodyErr != "" {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: 400,
				Msg:    bodyErr,
			})
			continue
		}

		// 3. Request hợp lệ -> ghi nhận thông tin và dồn lên đầu mảng
		uStart := len(s.usernameBuf)
		s.usernameBuf = append(s.usernameBuf, username...)
		task.Username = s.usernameBuf[uStart:len(s.usernameBuf)]

		task.UserID = int64(userID)
		task.Body = body
		tasks[validCount] = *task
		validCount++
	}

	tasks = tasks[:validCount]
	batch.CreatePostTasks = tasks

	if validCount == 0 {
		return
	}

	if validCount > len(s.userIDs) {
		s.userIDs = make([]int64, validCount)
		s.bodyOffsets = make([]int32, validCount)
		s.bodyLens = make([]int32, validCount)
		s.postIDs = make([]int64, validCount)
		s.statuses = make([]int32, validCount)
	}

	s.bodyBuf = s.bodyBuf[:0]
	for i := 0; i < validCount; i++ {
		task := &tasks[i]
		s.userIDs[i] = task.UserID
		s.bodyOffsets[i] = int32(len(s.bodyBuf))
		s.bodyLens[i] = int32(len(task.Body))
		s.bodyBuf = append(s.bodyBuf, task.Body...)
	}

	nowUTC := time.Now().UTC()
	nowFormatted := nowUTC.AppendFormat(s.nowBuf[:0], "2006-01-02T15:04:05.000Z")

	err := sqlite.CreatePostsBatch(
		s.userIDs[:validCount],
		s.bodyBuf,
		s.bodyOffsets[:validCount],
		s.bodyLens[:validCount],
		nowFormatted,
		s.postIDs[:validCount],
		s.statuses[:validCount],
	)

	if err == nil {
		for i := 0; i < validCount; i++ {
			tasks[i].PostID = s.postIDs[i]
		}
	}

	s.respWriter.WriteCreatePosts(tasks, s.statuses[:validCount], nowFormatted, err, outTasks)
}

// =============================================================================
// 7. LIKE POST SYSTEM (TWO-POINTER IN-PLACE FILTERING & GROUP COMMIT CGO)
// =============================================================================

type LikePostSystem struct {
	db         *DB
	verifier   *JWTVerifier
	respWriter *ResponseWriter
	userIDs    []int64
	postIDs    []int64
	results    []int32
	nowBuf     [32]byte
}

func NewLikePostSystem(db *DB, respWriter *ResponseWriter, jwtSecret []byte) *LikePostSystem {
	return &LikePostSystem{
		db:         db,
		verifier:   NewJWTVerifier(jwtSecret),
		respWriter: respWriter,
		userIDs:    make([]int64, 4096),
		postIDs:    make([]int64, 4096),
		results:    make([]int32, 4096),
	}
}

func (s *LikePostSystem) Process(batch *EventBatch, outTasks *[]iouring.SendInfo) {
	tasks := batch.LikePostTasks
	if len(tasks) == 0 {
		return
	}

	now := time.Now().Unix()
	validCount := 0

	// Two-Pointer In-Place Filtering & Auth
	for i := range tasks {
		task := &tasks[i]

		// 1. Kiểm tra Auth TRƯỚC theo đúng SPEC
		if len(task.Token) == 0 {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: 401,
				Msg:    "missing bearer token",
			})
			continue
		}

		userID, _, authErr, status := s.verifier.Verify(task.Token, now)
		if authErr != "" {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: status,
				Msg:    authErr,
			})
			continue
		}

		// 2. Kiểm tra PostID SAU khi Auth hợp lệ
		if task.PostID <= 0 {
			batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
				FD:     task.FD,
				Status: 400,
				Msg:    "invalid post id",
			})
			continue
		}

		// 3. Request hợp lệ -> ghi nhận UserID và dồn lên đầu mảng
		task.UserID = int64(userID)
		tasks[validCount] = *task
		validCount++
	}

	tasks = tasks[:validCount]
	batch.LikePostTasks = tasks

	if validCount == 0 {
		return
	}

	if validCount > len(s.userIDs) {
		s.userIDs = make([]int64, validCount)
		s.postIDs = make([]int64, validCount)
		s.results = make([]int32, validCount)
	}

	for i := 0; i < validCount; i++ {
		s.userIDs[i] = tasks[i].UserID
		s.postIDs[i] = tasks[i].PostID
	}

	nowUTC := time.Now().UTC()
	nowFormatted := nowUTC.AppendFormat(s.nowBuf[:0], "2006-01-02T15:04:05.000Z")

	err := sqlite.InsertLikesBatch(s.userIDs[:validCount], s.postIDs[:validCount], nowFormatted, s.results[:validCount])
	s.respWriter.WriteLikePosts(tasks, s.results[:validCount], err, outTasks)
}

// =============================================================================
// 8. SYSTEM MANAGER (ĐIỀU PHỐI LIFECYCLE, BUSINESS SYSTEMS & RESPONSE WRITER)
// =============================================================================

type SystemManager struct {
	ConnSys       *ConnectionSystem
	HealthSys     *HealthSystem
	FeedSys       *FeedSystem
	GetPostSys    *GetPostSystem
	CreatePostSys *CreatePostSystem
	LikePostSys   *LikePostSystem
	RespWriter    *ResponseWriter
}

func NewSystemManager(db *DB, sendPool *BufferPool, jwtSecret []byte) *SystemManager {
	respWriter := NewResponseWriter(sendPool)
	return &SystemManager{
		ConnSys:       NewConnectionSystem(),
		HealthSys:     NewHealthSystem(db, respWriter),
		FeedSys:       NewFeedSystem(db, respWriter),
		GetPostSys:    NewGetPostSystem(db, respWriter),
		CreatePostSys: NewCreatePostSystem(db, respWriter, jwtSecret),
		LikePostSys:   NewLikePostSystem(db, respWriter, jwtSecret),
		RespWriter:    respWriter,
	}
}

func (m *SystemManager) Dispatch(batch *EventBatch, outTasks *[]iouring.SendInfo) {
	// 1. Connection lifecycle (Accept, Rearm, Close)
	m.ConnSys.ProcessLifecycle(batch, outTasks)

	// 2. Business Systems (Two-Pointer In-Place Filtering & Auth -> DB CGO Batch -> Centralized Response Writer)
	if len(batch.HealthTasks) > 0 {
		m.HealthSys.Process(batch.HealthTasks, outTasks)
	}
	if len(batch.FeedTasks) > 0 {
		m.FeedSys.Process(batch.FeedTasks, outTasks)
	}
	if len(batch.GetPostTasks) > 0 {
		m.GetPostSys.Process(batch, outTasks)
	}
	if len(batch.CreatePostTasks) > 0 {
		m.CreatePostSys.Process(batch, outTasks)
	}
	if len(batch.LikePostTasks) > 0 {
		m.LikePostSys.Process(batch, outTasks)
	}

	// 3. Centralized Response Writer xuất toàn bộ ErrorTasks (từ Parser và các Systems)
	if len(batch.ErrorTasks) > 0 {
		m.RespWriter.WriteErrors(batch.ErrorTasks, outTasks)
	}
}
