package httpserver

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"time"

	iouring "go-iouring-quananh2503/src/io-uring"
)

// =============================================================================
// 1. ĐỊNH NGHĨA USERDATA & EVENT TYPES
// =============================================================================

type EventType int

const (
	EventAccept EventType = 1
	EventRecv   EventType = 2
	EventSend   EventType = 3
	EventClose  EventType = 4
)

// EncodeUserData đóng gói EventType, BufferID và FD vào uint64
func EncodeUserData(eventType EventType, bufID uint32, fd int32) uint64 {
	return (uint64(eventType) << 56) | (uint64(bufID&0xFFFFFF) << 32) | uint64(uint32(fd))
}

// DecodeUserData giải mã uint64 thành EventType, BufferID và FD
func DecodeUserData(userData uint64) (eventType EventType, bufID uint32, fd int32) {
	eventType = EventType(userData >> 56)
	bufID = uint32(userData>>32) & 0xFFFFFF
	fd = int32(userData)
	return
}

// =============================================================================
// 2. CÁC ĐỊNH NGHĨA EVENT SẠCH (CLEAN STRUCTS)
// =============================================================================

type AcceptEvent struct {
	ListenFD int32
	ClientFD int32
}

type HealthEvent struct {
	FD int32
}

type FeedEvent struct {
	FD int32
}

type GetPostEvent struct {
	FD     int32
	PostID int
}

type CreatePostEvent struct {
	FD       int32
	UserID   int
	Username []byte // Slice trỏ trực tiếp vào payload
	Body     []byte // Slice trỏ trực tiếp vào body
}

type LikePostEvent struct {
	FD     int32
	UserID int
	PostID int
}

type EventBatch struct {
	AcceptEvents     []AcceptEvent
	HealthEvents     []HealthEvent
	FeedEvents       []FeedEvent
	GetPostEvents    []GetPostEvent
	CreatePostEvents []CreatePostEvent
	LikePostEvents   []LikePostEvent
}

func NewEventBatch(cap int) *EventBatch {
	return &EventBatch{
		AcceptEvents:     make([]AcceptEvent, 0, cap),
		HealthEvents:     make([]HealthEvent, 0, cap),
		FeedEvents:       make([]FeedEvent, 0, cap),
		GetPostEvents:    make([]GetPostEvent, 0, cap),
		CreatePostEvents: make([]CreatePostEvent, 0, cap),
		LikePostEvents:   make([]LikePostEvent, 0, cap),
	}
}

func (b *EventBatch) Reset() {
	b.AcceptEvents = b.AcceptEvents[:0]
	b.HealthEvents = b.HealthEvents[:0]
	b.FeedEvents = b.FeedEvents[:0]
	b.GetPostEvents = b.GetPostEvents[:0]
	b.CreatePostEvents = b.CreatePostEvents[:0]
	b.LikePostEvents = b.LikePostEvents[:0]
}

// =============================================================================
// 3. PARSER CHÍNH (SINGLE-PASS IN-CACHE)
// =============================================================================

type Parser struct {
	BufRing   *iouring.ProvidedBufRing
	SendPool  *BufferPool
	JWTSecret []byte
}

func NewParser(bufRing *iouring.ProvidedBufRing, sendPool *BufferPool, jwtSecret []byte) *Parser {
	return &Parser{
		BufRing:   bufRing,
		SendPool:  sendPool,
		JWTSecret: jwtSecret,
	}
}

// Parse gặt mảng ReadInfo từ Kernel và phân loại vào EventBatch
func (p *Parser) Parse(tasks []iouring.ReadInfo, batch *EventBatch, outTasks *[]iouring.SendInfo) {
	batch.Reset()
	now := time.Now().Unix()

	for i := range tasks {
		task := &tasks[i]
		eventType, bufID, fd := DecodeUserData(task.UserData)

		switch eventType {
		// -------------------------------------------------------------
		// A. SỰ KIỆN GỬI HOÀN TẤT
		// -------------------------------------------------------------
		case EventSend:
			p.SendPool.Release(bufID) // Giải phóng send buffer về pool
			if task.Res < 0 {
				// Client đã ngắt kết nối giữa chừng -> đóng fixed socket
				p.closeConn(fd, outTasks)
			}
			// Multishot Recv vẫn đang thường trực trong kernel! Không cần rearmRecv!

		// -------------------------------------------------------------
		// B. SỰ KIỆN KẾT NỐI MỚI (ACCEPT MULTISHOT)
		// -------------------------------------------------------------
		case EventAccept:
			if task.Res >= 0 {
				batch.AcceptEvents = append(batch.AcceptEvents, AcceptEvent{
					ListenFD: fd,
					ClientFD: task.Res,
				})
			}
			// Nếu cờ IORING_CQE_F_MORE bị tắt, re-arm lại multishot accept
			if task.Flags&iouring.IORING_CQE_F_MORE == 0 {
				p.rearmAccept(fd, outTasks)
			}

		// -------------------------------------------------------------
		// C. SỰ KIỆN NHẬN DỮ LIỆU TỪ SOCKET (RECV MULTISHOT)
		// -------------------------------------------------------------
		case EventRecv:
			// 1. Kiểm tra ngắt kết nối hoặc lỗi mạng
			if task.Res <= 0 || task.Flags&iouring.IORING_CQE_F_BUFFER == 0 {
				if task.Flags&iouring.IORING_CQE_F_BUFFER != 0 {
					p.BufRing.Release(uint16(task.Flags >> 16))
				}
				p.closeConn(fd, outTasks)
				continue
			}

			// 2. Lấy dữ liệu raw bytes từ Provided Buffer Ring
			bid := uint16(task.Flags >> 16)
			raw := p.BufRing.GetSlice(bid, int(task.Res))

			// 3. Quét Single-Pass toàn bộ request
			p.parseSingle(fd, raw, now, batch, outTasks)

			// 4. Giải phóng buffer ngay lập tức về lại io_uring_buf_ring
			p.BufRing.Release(bid)

			// 5. Nếu kernel báo hết multishot (không còn cờ F_MORE), re-arm lại
			if task.Flags&iouring.IORING_CQE_F_MORE == 0 {
				p.rearmRecv(fd, outTasks)
			}

		// -------------------------------------------------------------
		// D. SỰ KIỆN ĐÓNG SOCKET (CLOSE FIXED FILE)
		// -------------------------------------------------------------
		case EventClose:
			// Kernel đã giải phóng slot trong fixed file table
		}
	}
}

// parseSingle phân tích trọn vẹn 1 request trong khi dữ liệu vẫn còn nóng trong Cache
func (p *Parser) parseSingle(fd int32, raw []byte, now int64, batch *EventBatch, outTasks *[]iouring.SendInfo) {
	if len(raw) < 14 {
		p.replyError(fd, 400, "malformed request", outTasks)
		return
	}

	// 1. Phân loại theo HTTP Method
	if bytes.HasPrefix(raw, []byte("GET ")) {
		p.parseGET(fd, raw[4:], batch, outTasks)
	} else if bytes.HasPrefix(raw, []byte("POST ")) {
		p.parsePOST(fd, raw[5:], raw, now, batch, outTasks)
	} else {
		p.replyError(fd, 404, "not found", outTasks)
	}
}

// parseGET xử lý: /health, /feed, /posts/:id
func (p *Parser) parseGET(fd int32, pathPart []byte, batch *EventBatch, outTasks *[]iouring.SendInfo) {
	if len(pathPart) < 2 || pathPart[0] != '/' {
		p.replyError(fd, 404, "not found", outTasks)
		return
	}

	switch pathPart[1] {
	case 'h': // /health
		if bytes.HasPrefix(pathPart, []byte("/health")) {
			batch.HealthEvents = append(batch.HealthEvents, HealthEvent{FD: fd})
			return
		}
	case 'f': // /feed
		if bytes.HasPrefix(pathPart, []byte("/feed")) {
			batch.FeedEvents = append(batch.FeedEvents, FeedEvent{FD: fd})
			return
		}
	case 'p': // /posts/:id
		if bytes.HasPrefix(pathPart, []byte("/posts/")) {
			idBytes := pathPart[7:]
			end := bytes.IndexByte(idBytes, ' ')
			if end >= 0 {
				idBytes = idBytes[:end]
			}
			postID, ok := parsePositiveInt(idBytes)
			if !ok {
				p.replyError(fd, 400, "invalid post id", outTasks)
				return
			}
			batch.GetPostEvents = append(batch.GetPostEvents, GetPostEvent{FD: fd, PostID: postID})
			return
		}
	}

	p.replyError(fd, 404, "not found", outTasks)
}

// parsePOST xử lý: POST /posts, POST /posts/:id/like
func (p *Parser) parsePOST(fd int32, pathPart []byte, raw []byte, now int64, batch *EventBatch, outTasks *[]iouring.SendInfo) {
	if !bytes.HasPrefix(pathPart, []byte("/posts")) {
		p.replyError(fd, 404, "not found", outTasks)
		return
	}

	// [QUY TẮC SPEC]: Phải kiểm tra Auth TRƯỚC, rồi mới kiểm tra ID/Body!
	token := extractBearerToken(raw)
	if len(token) == 0 {
		p.replyError(fd, 401, "missing bearer token", outTasks)
		return
	}

	// Verify chữ ký HMAC và bóc UserID, Username ngay tại chỗ
	userID, username, authErr, status := verifyJWT(token, p.JWTSecret, now)
	if authErr != "" {
		p.replyError(fd, status, authErr, outTasks)
		return
	}

	afterPosts := pathPart[6:] // Phần còn lại sau "/posts"

	// Case 1: POST /posts (Tạo bài viết mới)
	if len(afterPosts) == 0 || afterPosts[0] == ' ' || afterPosts[0] == '?' {
		body, bodyErr := extractJSONBodyField(raw)
		if bodyErr != "" {
			p.replyError(fd, 400, bodyErr, outTasks)
			return
		}
		batch.CreatePostEvents = append(batch.CreatePostEvents, CreatePostEvent{
			FD:       fd,
			UserID:   userID,
			Username: username,
			Body:     body,
		})
		return
	}

	// Case 2: POST /posts/:id/like (Like bài viết)
	if afterPosts[0] == '/' {
		likeIdx := bytes.Index(afterPosts, []byte("/like"))
		if likeIdx > 1 {
			idBytes := afterPosts[1:likeIdx]
			postID, ok := parsePositiveInt(idBytes)
			if !ok {
				p.replyError(fd, 400, "invalid post id", outTasks)
				return
			}
			batch.LikePostEvents = append(batch.LikePostEvents, LikePostEvent{
				FD:     fd,
				UserID: userID,
				PostID: postID,
			})
			return
		}
	}

	p.replyError(fd, 404, "not found", outTasks)
}

// =============================================================================
// 4. TIỆN ÍCH TRẢ LỖI VÀ RE-ARM RING
// =============================================================================

func (p *Parser) sendError(fd int32, statusText string, body string, outTasks *[]iouring.SendInfo) {
	sendBufID, err := p.SendPool.Acquire()
	if err != nil {
		p.closeConn(fd, outTasks)
		return
	}
	buf := p.SendPool.Get(sendBufID)

	resp := fmt.Sprintf("HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n%s",
		statusText, len(body), body)

	n := copy(buf, resp)

	*outTasks = append(*outTasks, iouring.SendInfo{
		Opcode:   iouring.OpSend,
		Flags:    iouring.IOSQE_FIXED_FILE,
		FD:       fd,
		Buf:      buf[:n],
		UserData: EncodeUserData(EventSend, sendBufID, fd),
	})
}

// replyError gửi ngay mã lỗi HTTP về cho client
func (p *Parser) replyError(fd int32, status int, msg string, outTasks *[]iouring.SendInfo) {
	body := fmt.Sprintf(`{"error":"%s"}`, msg)
	statusText := "400 Bad Request"
	switch status {
	case 401:
		statusText = "401 Unauthorized"
	case 404:
		statusText = "404 Not Found"
	case 500:
		statusText = "500 Internal Server Error"
	}

	p.sendError(fd, statusText, body, outTasks)
}

func (p *Parser) rearmRecv(fixedFD int32, outTasks *[]iouring.SendInfo) {
	*outTasks = append(*outTasks, iouring.SendInfo{
		Opcode:   iouring.OpRecv,
		Flags:    iouring.IOSQE_FIXED_FILE | iouring.IOSQE_BUFFER_SELECT,
		IOPrio:   iouring.IORING_RECV_MULTISHOT,
		FD:       fixedFD,
		BufIndex: 0,
		UserData: EncodeUserData(EventRecv, 0, fixedFD),
	})
}

func (p *Parser) rearmAccept(listenFD int32, outTasks *[]iouring.SendInfo) {
	*outTasks = append(*outTasks, iouring.SendInfo{
		Opcode:     iouring.OpAccept,
		FD:         listenFD,
		IOPrio:     iouring.IORING_ACCEPT_MULTISHOT,
		SpliceFDIn: iouring.IORING_FILE_INDEX_ALLOC,
		UserData:   EncodeUserData(EventAccept, 0, listenFD),
	})
}

func (p *Parser) closeConn(fixedFD int32, outTasks *[]iouring.SendInfo) {
	*outTasks = append(*outTasks, iouring.SendInfo{
		Opcode:     iouring.OpClose,
		SpliceFDIn: fixedFD + 1,
		UserData:   EncodeUserData(EventClose, 0, fixedFD),
	})
}

// =============================================================================
// 5. CÁC HÀM TIỆN ÍCH ZERO ALLOCATION
// =============================================================================

// parsePositiveInt chuyển đổi byte slice thành số nguyên dương (không allocation)
func parsePositiveInt(b []byte) (int, bool) {
	if len(b) == 0 {
		return 0, false
	}
	n := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int(c-'0')
	}
	if n <= 0 {
		return 0, false
	}
	return n, true
}

// extractBearerToken trích xuất token từ Authorization: Bearer <token>
func extractBearerToken(raw []byte) []byte {
	bearerPrefix := []byte("\r\nAuthorization: Bearer ")
	idx := bytes.Index(raw, bearerPrefix)
	if idx < 0 {
		if bytes.HasPrefix(raw, []byte("Authorization: Bearer ")) {
			start := len("Authorization: Bearer ")
			end := bytes.Index(raw[start:], []byte("\r\n"))
			if end >= 0 {
				return bytes.TrimSpace(raw[start : start+end])
			}
			return bytes.TrimSpace(raw[start:])
		}
		return nil
	}
	start := idx + len(bearerPrefix)
	end := bytes.Index(raw[start:], []byte("\r\n"))
	if end < 0 {
		return bytes.TrimSpace(raw[start:])
	}
	return bytes.TrimSpace(raw[start : start+end])
}

// extractJSONBodyField trích xuất an toàn trường "body" trong JSON
func extractJSONBodyField(raw []byte) ([]byte, string) {
	headerEnd := bytes.Index(raw, []byte("\r\n\r\n"))
	if headerEnd < 0 {
		return nil, "malformed JSON body"
	}
	bodyRaw := bytes.TrimSpace(raw[headerEnd+4:])
	if len(bodyRaw) == 0 {
		return nil, "body is required"
	}
	if bodyRaw[0] != '{' || bodyRaw[len(bodyRaw)-1] != '}' {
		return nil, "malformed JSON body"
	}

	key := []byte(`"body"`)
	keyIdx := bytes.Index(bodyRaw, key)
	if keyIdx < 0 {
		return nil, "body is required"
	}

	afterKey := bodyRaw[keyIdx+len(key):]
	colonIdx := bytes.IndexByte(afterKey, ':')
	if colonIdx < 0 {
		return nil, "malformed JSON body"
	}

	val := bytes.TrimSpace(afterKey[colonIdx+1:])
	if len(val) == 0 {
		return nil, "body is required"
	}

	// Nếu body là số -> không phải string
	if val[0] != '"' {
		return nil, "body is required"
	}

	// Quét tìm closing quote có tính đến ký tự escape (\")
	endQuote := -1
	escaped := false
	for i := 1; i < len(val); i++ {
		if escaped {
			escaped = false
			continue
		}
		if val[i] == '\\' {
			escaped = true
			continue
		}
		if val[i] == '"' {
			endQuote = i
			break
		}
	}

	if endQuote < 0 {
		return nil, "malformed JSON body"
	}
	strVal := val[1:endQuote]

	// Trim khoảng trắng theo yêu cầu SPEC
	trimmed := bytes.TrimSpace(strVal)
	if len(trimmed) == 0 {
		return nil, "body is required"
	}
	if len(trimmed) > 500 {
		return nil, "body must be at most 500 characters"
	}

	return trimmed, ""
}

// verifyJWT xác thực chữ ký HS256 và bóc tách claims
func verifyJWT(token []byte, secret []byte, now int64) (userID int, username []byte, errMsg string, status int) {
	parts := bytes.Split(token, []byte("."))
	if len(parts) != 3 {
		return 0, nil, "invalid or expired token", 401
	}

	headerB64 := parts[0]
	payloadB64 := parts[1]
	sigB64 := parts[2]

	// 1. Xác thực chữ ký HMAC-SHA256
	mac := hmac.New(sha256.New, secret)
	mac.Write(headerB64)
	mac.Write([]byte("."))
	mac.Write(payloadB64)
	expectedMAC := mac.Sum(nil)

	sigLen := base64.RawURLEncoding.DecodedLen(len(sigB64))
	sigBuf := make([]byte, sigLen)
	n, err := base64.RawURLEncoding.Decode(sigBuf, sigB64)
	if err != nil || !hmac.Equal(sigBuf[:n], expectedMAC) {
		return 0, nil, "invalid or expired token", 401
	}

	// 2. Giải mã Payload
	payloadLen := base64.RawURLEncoding.DecodedLen(len(payloadB64))
	payload := make([]byte, payloadLen)
	pn, err := base64.RawURLEncoding.Decode(payload, payloadB64)
	if err != nil {
		return 0, nil, "invalid token payload", 401
	}
	payload = payload[:pn]

	// 3. Trích xuất exp
	expKey := []byte(`"exp":`)
	if idx := bytes.Index(payload, expKey); idx >= 0 {
		val := bytes.TrimSpace(payload[idx+len(expKey):])
		expNum := 0
		for _, c := range val {
			if c < '0' || c > '9' {
				break
			}
			expNum = expNum*10 + int(c-'0')
		}
		if int64(expNum) < now {
			return 0, nil, "invalid or expired token", 401
		}
	}

	// 4. Trích xuất sub (User ID)
	subKey := []byte(`"sub":"`)
	subIdx := bytes.Index(payload, subKey)
	if subIdx < 0 {
		return 0, nil, "invalid token payload", 401
	}
	subVal := payload[subIdx+len(subKey):]
	endSub := bytes.IndexByte(subVal, '"')
	if endSub < 0 {
		return 0, nil, "invalid token payload", 401
	}
	uid, ok := parsePositiveInt(subVal[:endSub])
	if !ok {
		return 0, nil, "invalid token payload", 401
	}
	userID = uid

	// 5. Trích xuất username
	nameKey := []byte(`"username":"`)
	nameIdx := bytes.Index(payload, nameKey)
	if nameIdx < 0 {
		return 0, nil, "invalid token payload", 401
	}
	nameVal := payload[nameIdx+len(nameKey):]
	endName := bytes.IndexByte(nameVal, '"')
	if endName < 0 {
		return 0, nil, "invalid token payload", 401
	}
	username = nameVal[:endName]

	return userID, username, "", 200
}
