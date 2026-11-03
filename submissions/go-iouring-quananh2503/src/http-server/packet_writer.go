package httpserver

// PacketWriter quản lý một mảng byte cấp phát trước với con trỏ Pos,
// kế thừa trực tiếp chiến thuật zero-allocation từ ref/game-moba.
type PacketWriter struct {
	Buf []byte
	Pos int
}

// NewPacketWriter khởi tạo PacketWriter với dung lượng cố định
func NewPacketWriter(capacity int) *PacketWriter {
	return &PacketWriter{
		Buf: make([]byte, capacity),
		Pos: 0,
	}
}

// Reset đưa con trỏ vị trí về 0 trong O(1), tái sử dụng hoàn toàn bộ nhớ đệm
func (w *PacketWriter) Reset() {
	w.Pos = 0
}

// WriteByte ghi 1 byte trực tiếp (chuẩn io.ByteWriter)
func (w *PacketWriter) WriteByte(b byte) error {
	w.Buf[w.Pos] = b
	w.Pos++
	return nil
}

// WriteBytes ghi mảng byte dùng hàm copy (tương đương memmove trong Assembly)
func (w *PacketWriter) WriteBytes(payload []byte) {
	n := copy(w.Buf[w.Pos:], payload)
	w.Pos += n
}

// WriteString ghi chuỗi string trực tiếp không cấp phát heap
func (w *PacketWriter) WriteString(s string) {
	n := copy(w.Buf[w.Pos:], s)
	w.Pos += n
}

// WriteInt ghi số nguyên dương dạng ký tự ASCII không sinh garbage (0 mallocs)
func (w *PacketWriter) WriteInt(n int) {
	if n == 0 {
		w.Buf[w.Pos] = '0'
		w.Pos++
		return
	}
	if n < 0 {
		w.Buf[w.Pos] = '-'
		w.Pos++
		n = -n
	}
	var tmp [20]byte
	i := len(tmp)
	for n > 0 {
		i--
		tmp[i] = byte('0' + n%10)
		n /= 10
	}
	w.WriteBytes(tmp[i:])
}

// Bytes trả về slice dữ liệu đã ghi từ 0 đến Pos
func (w *PacketWriter) Bytes() []byte {
	return w.Buf[:w.Pos]
}

// Len trả về số byte hiện tại
func (w *PacketWriter) Len() int {
	return w.Pos
}
