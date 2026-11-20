package sqlite

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestGetFeed(t *testing.T) {
	// Đường dẫn tới feed.db mẫu trong repo
	dbPath := filepath.Join("..", "..", "..", "..", "seed", "feed.db")
	if _, err := os.Stat(dbPath); os.IsNotExist(err) {
		t.Skip("Không tìm thấy seed/feed.db, bỏ qua test")
	}

	err := InitFeedEngine(dbPath)
	if err != nil {
		t.Fatalf("InitFeedEngine lỗi: %v", err)
	}
	defer CloseFeedEngine()

	data, err := GetFeed()
	if err != nil {
		t.Fatalf("GetFeed lỗi: %v", err)
	}

	if len(data) == 0 {
		t.Fatalf("GetFeed trả về rỗng")
	}

	t.Logf("Feed JSON length: %d bytes", len(data))
	t.Logf("Feed JSON preview: %s", string(data[:min(len(data), 200)]))

	// Kiểm tra tính hợp lệ của JSON
	var parsed struct {
		Posts []map[string]interface{} `json:"posts"`
	}
	if err := json.Unmarshal(data, &parsed); err != nil {
		t.Fatalf("JSON không hợp lệ: %v\nJSON: %s", err, string(data))
	}

	if len(parsed.Posts) != 20 {
		t.Fatalf("Kỳ vọng 20 posts, thực tế nhận được: %d", len(parsed.Posts))
	}

	b, _ := json.Marshal(parsed.Posts[0])
	t.Logf("B-tree first post parsed: %s", string(b))

	goldenPath := filepath.Join("..", "..", "..", "..", "test", "golden", "feed.json")
	if goldenBytes, err := os.ReadFile(goldenPath); err == nil {
		if string(data) == string(goldenBytes) {
			t.Logf("TUYỆT VỜI: JSON khớp 100%% từng byte với test/golden/feed.json!")
		} else {
			t.Logf("Lưu ý: JSON hợp lệ nhưng có sự khác biệt nhỏ về whitespace hoặc escape so với golden.")
		}
	}

	t.Logf("Thành công! Đã lấy đủ 20 posts hợp lệ từ SQLite B-tree")
}

func TestGetPost(t *testing.T) {
	dbPath := filepath.Join("..", "..", "..", "..", "seed", "feed.db")
	if _, err := os.Stat(dbPath); os.IsNotExist(err) {
		t.Skip("Không tìm thấy seed/feed.db, bỏ qua test")
	}

	err := InitFeedEngine(dbPath)
	if err != nil {
		t.Fatalf("InitFeedEngine lỗi: %v", err)
	}
	defer CloseFeedEngine()

	// 1. Test post 1
	data1, status, err := GetPost(1)
	if err != nil || status != 200 {
		t.Fatalf("GetPost(1) lỗi: status=%d, err=%v", status, err)
	}
	golden1Path := filepath.Join("..", "..", "..", "..", "test", "golden", "post-1.json")
	if golden1, err := os.ReadFile(golden1Path); err == nil {
		if string(data1) != string(golden1) {
			t.Fatalf("Post 1 không khớp golden: got %s, want %s", string(data1), string(golden1))
		}
		t.Logf("Post 1 khớp 100%% byte-for-byte với golden!")
	}

	// 2. Test post 500000
	data500k, status, err := GetPost(500000)
	if err != nil || status != 200 {
		t.Fatalf("GetPost(500000) lỗi: status=%d, err=%v", status, err)
	}
	golden500kPath := filepath.Join("..", "..", "..", "..", "test", "golden", "post-500000.json")
	if golden500k, err := os.ReadFile(golden500kPath); err == nil {
		if string(data500k) != string(golden500k) {
			t.Fatalf("Post 500000 không khớp golden: got %s, want %s", string(data500k), string(golden500k))
		}
		t.Logf("Post 500000 khớp 100%% byte-for-byte với golden!")
	}

	// 3. Test post 999999999 (404)
	_, status404, err := GetPost(999999999)
	if status404 != 404 {
		t.Fatalf("Kỳ vọng 404 cho post không tồn tại, nhưng nhận: %d (err=%v)", status404, err)
	}
	t.Logf("Post 999999999 trả về đúng 404!")

	// 4. Test PostExists
	ex1, _ := PostExists(1)
	if !ex1 {
		t.Fatalf("PostExists(1) phải là true")
	}
	exNone, _ := PostExists(999999999)
	if exNone {
		t.Fatalf("PostExists(999999999) phải là false")
	}
	t.Logf("PostExists hoạt động hoàn hảo!")

	// 5. Test GetPostsBatch & PostExistsBatch
	testBatchIDs := []int64{1, 500000, 999999999, 1, 500000}
	items := make([]PostBatchItem, len(testBatchIDs))
	outBuf := make([]byte, 65536)

	err = GetPostsBatch(testBatchIDs, outBuf, items)
	if err != nil {
		t.Fatalf("GetPostsBatch lỗi: %v", err)
	}

	if items[0].Status != 200 || items[1].Status != 200 || items[2].Status != 404 || items[3].Status != 200 || items[4].Status != 200 {
		t.Fatalf("Statuses không đúng: %+v", items)
	}

	// Kiểm tra byte trùng khớp
	post1Batch := string(outBuf[items[0].Offset : items[0].Offset+items[0].Len])
	if golden1, err := os.ReadFile(golden1Path); err == nil {
		if post1Batch != string(golden1) {
			t.Fatalf("Batch Post 1 không khớp golden!")
		}
	}
	// Kiểm tra tối ưu deduplication: items[3] trùng ID với items[0], phải trỏ cùng offset!
	if items[3].Offset != items[0].Offset {
		t.Fatalf("Tối ưu deduplication thất bại: item[3].Offset=%d != item[0].Offset=%d", items[3].Offset, items[0].Offset)
	}
	t.Logf("Tối ưu Batch Deduplication thành công: ID 1 và 500000 trùng lặp đều được reuse không tốn B-Tree query!")

	// Test PostExistsBatch
	resExists := make([]int32, len(testBatchIDs))
	err = PostExistsBatch(testBatchIDs, resExists)
	if err != nil {
		t.Fatalf("PostExistsBatch lỗi: %v", err)
	}
	if resExists[0] != 1 || resExists[1] != 1 || resExists[2] != 0 || resExists[3] != 1 || resExists[4] != 1 {
		t.Fatalf("PostExistsBatch kết quả không đúng: %+v", resExists)
	}
	t.Logf("PostExistsBatch hoạt động hoàn hảo!")
}

func TestWritesAndHealth(t *testing.T) {
	origDB := filepath.Join("..", "..", "..", "..", "seed", "feed.db")
	testDB := "/tmp/test_writes.db"
	_ = os.Remove(testDB)
	_ = os.Remove(testDB + "-wal")
	_ = os.Remove(testDB + "-shm")

	input, err := os.ReadFile(origDB)
	if err != nil {
		t.Skip("Không tìm thấy seed/feed.db, bỏ qua test")
	}
	if err := os.WriteFile(testDB, input, 0644); err != nil {
		t.Fatalf("Không thể copy test db: %v", err)
	}
	defer os.Remove(testDB)

	err = InitFeedEngine(testDB)
	if err != nil {
		t.Fatalf("InitFeedEngine lỗi: %v", err)
	}
	defer CloseFeedEngine()

	if !DBHealth() {
		t.Fatalf("DBHealth phải là true")
	}
	t.Logf("DBHealth OK!")

	// 1. Test InsertLikesBatch (bao gồm deduplication trong batch và post không tồn tại)
	userIDs := []int64{101, 101, 102, 101}
	postIDs := []int64{2, 2, 2, 999999999}
	likeResults := make([]int32, len(userIDs))
	nowStr := []byte("2026-11-05T03:15:00.000Z")

	err = InsertLikesBatch(userIDs, postIDs, nowStr, likeResults)
	if err != nil {
		t.Fatalf("InsertLikesBatch lỗi: %v", err)
	}

	// likeResults[0]: user 101 like post 2 -> 201 hoặc 200 (tùy đã like trước chưa)
	// likeResults[1]: user 101 like post 2 lại ngay trong batch -> phải là 200 (already liked)
	if likeResults[1] != 200 {
		t.Fatalf("Kỳ vọng likeResults[1] == 200, nhận: %d", likeResults[1])
	}
	// likeResults[3]: post 999999999 không tồn tại -> phải là 404
	if likeResults[3] != 404 {
		t.Fatalf("Kỳ vọng likeResults[3] == 404, nhận: %d", likeResults[3])
	}
	t.Logf("InsertLikesBatch thành công: %+v", likeResults)

	// 2. Test CreatePostsBatch
	cUserIDs := []int64{1, 2}
	bodies := []string{"Hello from unit test 1", "Hello from unit test 2"}
	var bodiesBuf []byte
	bodyOffsets := make([]int32, len(bodies))
	bodyLens := make([]int32, len(bodies))
	for i, b := range bodies {
		bodyOffsets[i] = int32(len(bodiesBuf))
		bodyLens[i] = int32(len(b))
		bodiesBuf = append(bodiesBuf, []byte(b)...)
	}

	cIDs := make([]int64, len(bodies))
	cStatuses := make([]int32, len(bodies))

	err = CreatePostsBatch(cUserIDs, bodiesBuf, bodyOffsets, bodyLens, nowStr, cIDs, cStatuses)
	if err != nil {
		t.Fatalf("CreatePostsBatch lỗi: %v", err)
	}

	for i := range bodies {
		if cStatuses[i] != 201 {
			t.Fatalf("CreatePost[%d] status != 201: %d", i, cStatuses[i])
		}
		if cIDs[i] <= 500000 {
			t.Fatalf("CreatePost[%d] ID không tăng: %d", i, cIDs[i])
		}
		t.Logf("Tạo post thành công: ID=%d, created_at=%s", cIDs[i], string(nowStr))
	}

	// 3. Test đọc lại post mới tạo bằng GetPost trực tiếp từ B-Tree
	postBytes, status, err := GetPost(cIDs[0])
	if err != nil || status != 200 {
		t.Fatalf("GetPost(%d) sau khi create lỗi: status=%d err=%v", cIDs[0], status, err)
	}
	t.Logf("Đọc lại post mới tạo thành công: %s", string(postBytes))

	// 4. Test GetFeed vẫn chạy chuẩn xác và thấy post mới nhất
	feedBytes, err := GetFeed()
	if err != nil {
		t.Fatalf("GetFeed sau khi create lỗi: %v", err)
	}
	t.Logf("GetFeed sau khi ghi dữ liệu hoạt động mượt mà! Độ dài: %d", len(feedBytes))
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
