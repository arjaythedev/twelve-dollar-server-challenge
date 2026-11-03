package iouring

import (
	"bytes"
	"net"
	"os"
	"syscall"
	"testing"
)

// TestNOPBatch: Kiểm tra gửi và nhận một mẻ lệnh NOP thật với Linux Kernel
func TestNOPBatch(t *testing.T) {

	ring, err := NewRing(16)
	if err != nil {
		t.Fatalf("Khởi tạo Ring thất bại: %v", err)
	}
	defer ring.Close()

	// 1. Chuẩn bị 3 task NOP với 3 tag khác nhau
	reqs := []SendInfo{
		{Opcode: OpNop, UserData: 101},
		{Opcode: OpNop, UserData: 102},
		{Opcode: OpNop, UserData: 103},
	}

	// 2. Gửi cả mẻ 3 task đi cùng lúc
	n, err := ring.SendTasks(reqs)
	if err != nil {
		t.Fatalf("SendTasks lỗi: %v", err)
	}
	if n != 3 {
		t.Fatalf("Muốn submit 3 task nhưng chỉ submit được %d", n)
	}

	// 3. Đọc cả mẻ kết quả về
	results := make([]ReadInfo, 16)
	count, err := ring.RecieveOutputTasks(results, 3) // Chờ đủ 3 kết quả
	if err != nil {
		t.Fatalf("RecieveOutputTasks lỗi: %v", err)
	}
	if count != 3 {
		t.Fatalf("Muốn nhận 3 kết quả nhưng chỉ nhận được %d", count)
	}

	// 4. Kiểm tra từng kết quả từ Kernel
	receivedTags := make(map[uint64]bool)
	for i := 0; i < count; i++ {
		res := results[i]
		if res.Res != 0 {
			t.Errorf("Task %d bị lỗi: Res = %d", res.UserData, res.Res)
		}
		receivedTags[res.UserData] = true
	}

	for _, tag := range []uint64{101, 102, 103} {
		if !receivedTags[tag] {
			t.Errorf("Chưa nhận được kết quả của tag %d", tag)
		}
	}
}

// TestFileWriteAndReadBatch: Kiểm tra ghi và đọc file thật qua Linux Kernel bằng io_uring
func TestFileWriteAndReadBatch(t *testing.T) {
	// Ghi trực tiếp ra file thật trong thư mục hiện tại để bạn có thể mở ra kiểm tra tận mắt!
	filePath := "test_io_uring_real.txt"

	f, err := os.OpenFile(filePath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		t.Fatalf("Không thể tạo file test: %v", err)
	}
	defer f.Close()
	// Không xóa file để bạn có thể mở file kiểm tra nội dung

	ring, err := NewRing(16)
	if err != nil {
		t.Fatalf("Khởi tạo Ring thất bại: %v", err)
	}
	defer ring.Close()

	writeContent := []byte("Xin chao! Day la file that duoc ghi boi io_uring tren Linux!\n")
	fd := int32(f.Fd())

	// ==========================================
	// BƯỚC 1: GHI FILE THẬT
	// ==========================================
	writeReqs := []SendInfo{
		{
			Opcode:   OpWrite,
			FD:       fd,
			Buf:      writeContent,
			Off:      0,
			UserData: 555,
		},
	}

	n, err := ring.SendTasks(writeReqs)
	if err != nil || n != 1 {
		t.Fatalf("SendTasks ghi file thất bại: err=%v, n=%d", err, n)
	}

	results := make([]ReadInfo, 8)
	count, err := ring.RecieveOutputTasks(results, 1)
	if err != nil || count != 1 {
		t.Fatalf("RecieveOutputTasks ghi file thất bại: err=%v, count=%d", err, count)
	}

	if results[0].UserData != 555 || results[0].Res != int32(len(writeContent)) {
		t.Fatalf("Kết quả ghi file sai: tag=%d, bytes=%d (kỳ vọng %d)",
			results[0].UserData, results[0].Res, len(writeContent))
	}

	// ==========================================
	// BƯỚC 2: ĐỌC LẠI FILE THẬT
	// ==========================================
	readBuf := make([]byte, len(writeContent))
	readReqs := []SendInfo{
		{
			Opcode:   OpRead,
			FD:       fd,
			Buf:      readBuf,
			Off:      0,
			UserData: 666,
		},
	}

	n, err = ring.SendTasks(readReqs)
	if err != nil || n != 1 {
		t.Fatalf("SendTasks đọc file thất bại: err=%v, n=%d", err, n)
	}

	count, err = ring.RecieveOutputTasks(results, 1)
	if err != nil || count != 1 {
		t.Fatalf("RecieveOutputTasks đọc file thất bại: err=%v, count=%d", err, count)
	}

	if results[0].UserData != 666 || results[0].Res != int32(len(writeContent)) {
		t.Fatalf("Kết quả đọc file sai: tag=%d, bytes=%d", results[0].UserData, results[0].Res)
	}

	if !bytes.Equal(readBuf, writeContent) {
		t.Fatalf("Dữ liệu đọc được không khớp: got %q, want %q", string(readBuf), string(writeContent))
	}
}

// TestSocketEchoBatch: Kiểm tra toàn diện mạng socket thật qua io_uring:
// OpAccept (chờ kết nối) -> OpRecv (nhận tin) -> OpSend (gửi tin phản hồi)
func TestSocketEchoBatch(t *testing.T) {
	// 1. Tạo TCP Listener trên cổng ngẫu nhiên của 127.0.0.1
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("Không thể tạo TCP listener: %v", err)
	}
	defer listener.Close()

	tcpListener := listener.(*net.TCPListener)
	file, err := tcpListener.File()
	if err != nil {
		t.Fatalf("Không thể lấy FD từ listener: %v", err)
	}
	defer file.Close()
	listenFD := int32(file.Fd())

	ring, err := NewRing(16)
	if err != nil {
		t.Fatalf("Khởi tạo Ring thất bại: %v", err)
	}
	defer ring.Close()

	// 2. Soạn task OpAccept đưa vào Ring bằng SendTasks
	acceptReq := []SendInfo{
		{Opcode: OpAccept, FD: listenFD, UserData: 100},
	}
	if _, err := ring.SendTasks(acceptReq); err != nil {
		t.Fatalf("SendTasks OpAccept thất bại: %v", err)
	}

	// 3. Client kết nối tới server ở một goroutine riêng
	clientMsg := "Hello io_uring! Tin nhan that qua mang socket TCP!"
	errChan := make(chan error, 1)
	respChan := make(chan string, 1)

	go func() {
		conn, err := net.Dial("tcp", listener.Addr().String())
		if err != nil {
			errChan <- err
			return
		}
		defer conn.Close()

		// Client gửi tin nhắn
		if _, err := conn.Write([]byte(clientMsg)); err != nil {
			errChan <- err
			return
		}

		// Client nhận phản hồi echo từ server
		buf := make([]byte, 1024)
		n, err := conn.Read(buf)
		if err != nil {
			errChan <- err
			return
		}
		respChan <- string(buf[:n])
	}()

	// 4. Server thu hoạch kết quả Accept từ Ring
	results := make([]ReadInfo, 8)
	n, err := ring.RecieveOutputTasks(results, 1) // Chờ client kết nối
	if err != nil || n != 1 || results[0].UserData != 100 || results[0].Res < 0 {
		t.Fatalf("OpAccept thất bại: err=%v, res=%+v", err, results[0])
	}

	clientFD := results[0].Res // Đây chính là FD socket của client do Kernel trả về!
	defer syscall.Close(int(clientFD))

	// 5. Server nộp task OpRecv để nhận tin nhắn từ clientFD
	recvBuf := make([]byte, 1024)
	recvReq := []SendInfo{
		{Opcode: OpRecv, FD: clientFD, Buf: recvBuf, UserData: 200},
	}
	if _, err := ring.SendTasks(recvReq); err != nil {
		t.Fatalf("SendTasks OpRecv thất bại: %v", err)
	}

	// 6. Server thu hoạch kết quả Recv
	n, err = ring.RecieveOutputTasks(results, 1)
	if err != nil || n != 1 || results[0].UserData != 200 || results[0].Res < 0 {
		t.Fatalf("OpRecv thất bại: err=%v, res=%+v", err, results[0])
	}

	receivedBytes := results[0].Res
	receivedMsg := string(recvBuf[:receivedBytes])
	if receivedMsg != clientMsg {
		t.Fatalf("Tin nhắn nhận được không đúng: got %q, want %q", receivedMsg, clientMsg)
	}

	// 7. Server gửi lại phản hồi bằng OpSend
	echoMsg := []byte("ECHO: " + receivedMsg)
	sendReq := []SendInfo{
		{Opcode: OpSend, FD: clientFD, Buf: echoMsg, UserData: 300},
	}
	if _, err := ring.SendTasks(sendReq); err != nil {
		t.Fatalf("SendTasks OpSend thất bại: %v", err)
	}

	// 8. Server thu hoạch kết quả Send (xác nhận Kernel đã gửi xong bao nhiêu byte)
	n, err = ring.RecieveOutputTasks(results, 1)
	if err != nil || n != 1 || results[0].UserData != 300 || results[0].Res != int32(len(echoMsg)) {
		t.Fatalf("OpSend thất bại: err=%v, res=%+v", err, results[0])
	}

	// 9. Xác nhận client đã nhận được đúng nội dung echo
	select {
	case err := <-errChan:
		t.Fatalf("Client gặp lỗi: %v", err)
	case resp := <-respChan:
		if resp != string(echoMsg) {
			t.Fatalf("Client nhận phản hồi sai: got %q, want %q", resp, string(echoMsg))
		}
	}
}
