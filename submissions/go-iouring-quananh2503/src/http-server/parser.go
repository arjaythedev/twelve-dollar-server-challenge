package httpserver

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"

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
// 2. CÁC ĐỊNH NGHĨA TASK & EVENT STRUCTS THEO CHUẨN DOD
// =============================================================================

type AcceptEvent struct {
	ListenFD int32
	ClientFD int32
}

type GetPostTask struct {
	FD     int32
	PostID int64
}

type LikePostTask struct {
	FD     int32
	PostID int64
	Token  []byte
	UserID int64
}

type CreatePostTask struct {
	FD       int32
	Token    []byte
	Body     []byte
	UserID   int64
	Username []byte
	PostID   int64
}

type ErrorTask struct {
	FD     int32
	Status int    // 400, 401, 404, 500
	Msg    string // "not found", "invalid post id", "missing bearer token"...
}

type EventBatch struct {
	AcceptEvents     []AcceptEvent
	HealthTasks      []int32
	FeedTasks        []int32
	GetPostTasks     []GetPostTask
	LikePostTasks    []LikePostTask
	CreatePostTasks  []CreatePostTask
	ErrorTasks       []ErrorTask
	CloseQueue       []int32
	RearmRecvQueue   []int32
	RearmAcceptQueue []int32
}

func NewEventBatch(cap int) *EventBatch {
	return &EventBatch{
		AcceptEvents:     make([]AcceptEvent, 0, cap),
		HealthTasks:      make([]int32, 0, cap),
		FeedTasks:        make([]int32, 0, cap),
		GetPostTasks:     make([]GetPostTask, 0, cap),
		LikePostTasks:    make([]LikePostTask, 0, cap),
		CreatePostTasks:  make([]CreatePostTask, 0, cap),
		ErrorTasks:       make([]ErrorTask, 0, cap),
		CloseQueue:       make([]int32, 0, cap),
		RearmRecvQueue:   make([]int32, 0, cap),
		RearmAcceptQueue: make([]int32, 0, cap),
	}
}

func (b *EventBatch) Reset() {
	b.AcceptEvents = b.AcceptEvents[:0]
	b.HealthTasks = b.HealthTasks[:0]
	b.FeedTasks = b.FeedTasks[:0]
	b.GetPostTasks = b.GetPostTasks[:0]
	b.LikePostTasks = b.LikePostTasks[:0]
	b.CreatePostTasks = b.CreatePostTasks[:0]
	b.ErrorTasks = b.ErrorTasks[:0]
	b.CloseQueue = b.CloseQueue[:0]
	b.RearmRecvQueue = b.RearmRecvQueue[:0]
	b.RearmAcceptQueue = b.RearmAcceptQueue[:0]
}

// =============================================================================
// 3. PARSER CHÍNH (ZERO SIDE-EFFECTS SINGLE-PASS)
// =============================================================================

type Parser struct {
	BufRing *iouring.ProvidedBufRing
}

func NewParser(bufRing *iouring.ProvidedBufRing) *Parser {
	return &Parser{
		BufRing: bufRing,
	}
}

// Parse gặt mảng ReadInfo từ Kernel và phân loại vào EventBatch sạch (Zero Side-Effects)
func (p *Parser) Parse(tasks []iouring.ReadInfo, batch *EventBatch) {
	batch.Reset()

	for i := range tasks {
		task := &tasks[i]
		eventType, _, fd := DecodeUserData(task.UserData)

		switch eventType {
		// -------------------------------------------------------------
		// A. SỰ KIỆN GỬI HOÀN TẤT (CHỈ NHẬN KHI LỖI DO SKIP_SUCCESS)
		// -------------------------------------------------------------
		case EventSend:
			if task.Res < 0 {
				// Client đã ngắt kết nối giữa chừng -> ghi nhận đóng fixed socket
				batch.CloseQueue = append(batch.CloseQueue, fd)
			}

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
			// Nếu cờ IORING_CQE_F_MORE bị tắt, ghi nhận cần re-arm lại multishot accept
			if task.Flags&iouring.IORING_CQE_F_MORE == 0 {
				batch.RearmAcceptQueue = append(batch.RearmAcceptQueue, fd)
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
				batch.CloseQueue = append(batch.CloseQueue, fd)
				continue
			}

			// 2. Lấy dữ liệu raw bytes từ Provided Buffer Ring
			bid := uint16(task.Flags >> 16)
			raw := p.BufRing.GetSlice(bid, int(task.Res))

			// 3. Quét Single-Pass trích xuất thông tin vào EventBatch (Zero Allocation)
			p.parseSingle(fd, raw, batch)

			// 4. Trả buffer về lại ring
			p.BufRing.Release(bid)

			// 5. Nếu kernel báo hết multishot (không còn cờ F_MORE), ghi nhận cần re-arm
			if task.Flags&iouring.IORING_CQE_F_MORE == 0 {
				batch.RearmRecvQueue = append(batch.RearmRecvQueue, fd)
			}

		// -------------------------------------------------------------
		// D. SỰ KIỆN ĐÓNG SOCKET
		// -------------------------------------------------------------
		case EventClose:
			// Đóng socket thành công đã skip CQE; CQE chỉ sinh khi lỗi và không cần xử lý gì thêm
		}
	}
}

// parseSingle phân tích 1 request và đẩy vào mảng compact struct tương ứng
func (p *Parser) parseSingle(fd int32, raw []byte, batch *EventBatch) {
	if len(raw) < 14 {
		batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
			FD:     fd,
			Status: 400,
			Msg:    "malformed request",
		})
		return
	}

	// 1. Phân loại theo HTTP Method
	if bytes.HasPrefix(raw, []byte("GET ")) {
		p.parseGET(fd, raw[4:], batch)
	} else if bytes.HasPrefix(raw, []byte("POST ")) {
		p.parsePOST(fd, raw[5:], raw, batch)
	} else {
		batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
			FD:     fd,
			Status: 404,
			Msg:    "not found",
		})
	}
}

// parseGET xử lý: /health, /feed, /posts/:id
func (p *Parser) parseGET(fd int32, pathPart []byte, batch *EventBatch) {
	if len(pathPart) < 2 || pathPart[0] != '/' {
		batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
			FD:     fd,
			Status: 404,
			Msg:    "not found",
		})
		return
	}

	switch pathPart[1] {
	case 'h': // /health
		if bytes.HasPrefix(pathPart, []byte("/health")) {
			batch.HealthTasks = append(batch.HealthTasks, fd)
			return
		}
	case 'f': // /feed
		if bytes.HasPrefix(pathPart, []byte("/feed")) {
			batch.FeedTasks = append(batch.FeedTasks, fd)
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
				batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
					FD:     fd,
					Status: 400,
					Msg:    "invalid post id",
				})
				return
			}
			batch.GetPostTasks = append(batch.GetPostTasks, GetPostTask{
				FD:     fd,
				PostID: int64(postID),
			})
			return
		}
	}

	batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
		FD:     fd,
		Status: 404,
		Msg:    "not found",
	})
}

// parsePOST xử lý: POST /posts, POST /posts/:id/like
func (p *Parser) parsePOST(fd int32, pathPart []byte, raw []byte, batch *EventBatch) {
	if !bytes.HasPrefix(pathPart, []byte("/posts")) {
		batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
			FD:     fd,
			Status: 404,
			Msg:    "not found",
		})
		return
	}

	token := extractBearerToken(raw)
	afterPosts := pathPart[6:] // Phần còn lại sau "/posts"

	// Case 1: POST /posts (Tạo bài viết mới)
	if len(afterPosts) == 0 || afterPosts[0] == ' ' || afterPosts[0] == '?' {
		batch.CreatePostTasks = append(batch.CreatePostTasks, CreatePostTask{
			FD:    fd,
			Token: token,
			Body:  raw,
		})
		return
	}

	// Case 2: POST /posts/:id/like (Like bài viết)
	if afterPosts[0] == '/' {
		likeIdx := bytes.Index(afterPosts, []byte("/like"))
		if likeIdx > 1 {
			idBytes := afterPosts[1:likeIdx]
			postID, ok := parsePositiveInt(idBytes)
			var id int64 = -1
			if ok {
				id = int64(postID)
			}
			batch.LikePostTasks = append(batch.LikePostTasks, LikePostTask{
				FD:     fd,
				PostID: id,
				Token:  token,
			})
			return
		}
	}

	batch.ErrorTasks = append(batch.ErrorTasks, ErrorTask{
		FD:     fd,
		Status: 404,
		Msg:    "not found",
	})
}

// =============================================================================
// 4. CÁC HÀM TIỆN ÍCH ZERO ALLOCATION (HELPER FUNCTIONS)
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

// JWTVerifier quản lý instance HMAC tiền khởi tạo và scratch buffers để bóc tách token với 0 allocation
type JWTVerifier struct {
	mac        hash.Hash
	payloadBuf [512]byte
	sigBuf     [64]byte
	macBuf     [32]byte
}

func NewJWTVerifier(secret []byte) *JWTVerifier {
	return &JWTVerifier{
		mac: hmac.New(sha256.New, secret),
	}
}

// Verify xác thực chữ ký HS256 và bóc tách claims với 0 cấp phát heap (tái sử dụng mac và buffers)
func (v *JWTVerifier) Verify(token []byte, now int64) (userID int, username []byte, errMsg string, status int) {
	dot1 := bytes.IndexByte(token, '.')
	if dot1 <= 0 {
		return 0, nil, "invalid or expired token", 401
	}
	rest := token[dot1+1:]
	dot2 := bytes.IndexByte(rest, '.')
	if dot2 <= 0 {
		return 0, nil, "invalid or expired token", 401
	}
	sigB64 := rest[dot2+1:]
	if len(sigB64) == 0 || bytes.IndexByte(sigB64, '.') >= 0 {
		return 0, nil, "invalid or expired token", 401
	}

	headerB64 := token[:dot1]
	payloadB64 := rest[:dot2]

	// 1. Xác thực chữ ký HMAC-SHA256 (Tái sử dụng v.mac qua mac.Reset() -> 0 malloc)
	v.mac.Reset()
	v.mac.Write(headerB64)
	v.mac.Write([]byte("."))
	v.mac.Write(payloadB64)
	expectedMAC := v.mac.Sum(v.macBuf[:0])

	sigLen := base64.RawURLEncoding.DecodedLen(len(sigB64))
	if sigLen > len(v.sigBuf) {
		return 0, nil, "invalid or expired token", 401
	}
	n, err := base64.RawURLEncoding.Decode(v.sigBuf[:], sigB64)
	if err != nil || !hmac.Equal(v.sigBuf[:n], expectedMAC) {
		return 0, nil, "invalid or expired token", 401
	}

	// 2. Giải mã Payload vào buffer tĩnh (0 malloc)
	payloadLen := base64.RawURLEncoding.DecodedLen(len(payloadB64))
	if payloadLen > len(v.payloadBuf) {
		return 0, nil, "invalid token payload", 401
	}
	pn, err := base64.RawURLEncoding.Decode(v.payloadBuf[:], payloadB64)
	if err != nil {
		return 0, nil, "invalid token payload", 401
	}
	payload := v.payloadBuf[:pn]

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
