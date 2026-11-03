#!/usr/bin/env bash
set -eo pipefail

if git rev-parse --show-toplevel >/dev/null 2>&1; then
    REPO_ROOT="$(git rev-parse --show-toplevel)"
else
    REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    while [ "$REPO_ROOT" != "/" ] && [ ! -f "$REPO_ROOT/schema.sql" ]; do
        REPO_ROOT="$(dirname "$REPO_ROOT")"
    done
fi

SUBMISSION_DIR="${REPO_ROOT}/submissions/go-iouring-quananh2503"

# Cấu hình chuẩn mô phỏng 50.000 VUs với ramp-up mượt mà 30s
VUS="${VUS:-20000}"
SLEEP_FACTOR="${SLEEP_FACTOR:-0.4}"
RAMP_UP="${RAMP_UP:-30s}"
DURATION="${DURATION:-30s}"
WAIT_BEFORE_PROF="${WAIT_BEFORE_PROF:-32}" # Chờ 32s (qua hết 30s ramp-up để đạt đỉnh ổn định)
PROFILE_SECS="${PROFILE_SECS:-20}"         # Lấy mẫu 20s khi tải đang giữ đỉnh
PPROF_PORT="${PPROF_PORT:-8080}"

SERVER_PID=""

cleanup_on_interrupt() {
    echo ""
    echo "--- 🛑 Dừng do người dùng ngắt (Ctrl+C) ---"
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    pkill -9 -f "go-iouring-quananh2503/bin/server" 2>/dev/null || true
    fuser -k 3000/tcp 6060/tcp 2>/dev/null || true
    rm -f /tmp/bench_feed.db /tmp/bench_feed.db-wal /tmp/bench_feed.db-shm
    exit 1
}
trap cleanup_on_interrupt INT TERM

echo "=========================================================="
echo "⚡ BENCHMARK CHUẨN + PROFILING TẢI ĐỈNH (50k VUs Simulation)"
echo "  - VUs:                 $VUS (SLEEP_FACTOR=$SLEEP_FACTOR ~ 50k VUs)"
echo "  - Ramp-up:             $RAMP_UP (Tăng dần đều, mượt mà)"
echo "  - Duration:            $DURATION (Giữ đỉnh)"
echo "  - Chờ trước khi đo:   ${WAIT_BEFORE_PROF}s"
echo "  - Thời gian đo pprof:  ${PROFILE_SECS}s (Lấy mẫu lúc tải đỉnh)"
echo "  - Port Web UI pprof:   $PPROF_PORT"
echo "=========================================================="

# 0. Dọn dẹp tiến trình cũ
echo "0. Dọn dẹp tiến trình cũ..."
pkill -9 -f "go-iouring-quananh2503/bin/server" 2>/dev/null || true
fuser -k 3000/tcp 6060/tcp 2>/dev/null || true
sleep 1

# 1. Chuẩn bị database sạch ở /tmp
echo "1. Chuẩn bị database sạch tại /tmp/bench_feed.db..."
rm -f /tmp/bench_feed.db /tmp/bench_feed.db-wal /tmp/bench_feed.db-shm
cp "${REPO_ROOT}/seed/feed.db" /tmp/bench_feed.db

# 2. Build server với debug symbols
echo "2. Biên dịch server (giữ DWARF symbols để xem Source Code và Flame Graph)..."
cd "$SUBMISSION_DIR"
mkdir -p bin
export CGO_ENABLED=1
export CGO_CFLAGS="-O3 -DNDEBUG"
go build -o bin/server .
echo "   ✅ Build xong bin/server"

# 3. Khởi động server với 1 vCPU
echo "3. Khởi động server với 1 vCPU (GOMAXPROCS=1 taskset -c 0)..."
HOST=127.0.0.1 PORT=3000 SQLITE_PATH=/tmp/bench_feed.db JWT_SECRET=twelve-dollar-challenge CGO_ENABLED=1 GOMAXPROCS=1 taskset -c 0 "${SUBMISSION_DIR}/bin/server" > /tmp/server_bench_quananh.log 2>&1 &
SERVER_PID=$!
echo "   Server đã khởi động với PID: $SERVER_PID"

# 4. Kiểm tra health check
echo "4. Kiểm tra health check (http://127.0.0.1:3000/health)..."
ONLINE=0
for i in $(seq 1 30); do
    if curl -s -f http://127.0.0.1:3000/health > /dev/null 2>&1; then
        echo "   ✅ Server phản hồi HTTP 200 OK sau $i lần thử!"
        ONLINE=1
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "   ❌ Server tiến trình đã dừng đột ngột! Log lỗi:"
        cat /tmp/server_bench_quananh.log
        exit 1
    fi
    sleep 0.5
done

if [ "$ONLINE" -ne 1 ]; then
    echo "   ❌ Server không phản hồi /health kịp thời!"
    cat /tmp/server_bench_quananh.log
    exit 1
fi

# 5. Kích hoạt timer lấy mẫu CPU pprof trong lúc giữ tải đỉnh
(
    sleep "$WAIT_BEFORE_PROF"
    echo ""
    echo "🔥 [$(date +%T)] k6 ĐÃ QUA 30s RAMP-UP VÀ ĐẠT ĐỈNH ỔN ĐỊNH!"
    echo "🔥 [$(date +%T)] BẮT ĐẦU THU THẬP PROFILE TRONG ${PROFILE_SECS}s..."
    curl -s "http://127.0.0.1:6060/debug/pprof/profile?seconds=${PROFILE_SECS}" -o "${SUBMISSION_DIR}/cpu.prof"
    echo "✅ [$(date +%T)] ĐÃ THU THẬP XONG CPU PROFILE (${PROFILE_SECS}s)!"
) &
PPROF_BG_PID=$!

# 6. Chạy k6 toàn bộ chu trình và in kết quả đầy đủ
echo "5. Bắt đầu chạy k6 (30s ramp-up -> 30s giữ đỉnh -> in kết quả)..."
K6_BIN=$(which k6 || echo "${REPO_ROOT}/bin/k6")
taskset -c 1-15 "$K6_BIN" run \
  -e BASE_URL=http://127.0.0.1:3000 \
  -e VUS="$VUS" \
  -e SLEEP_FACTOR="$SLEEP_FACTOR" \
  -e RAMP_UP="$RAMP_UP" \
  -e DURATION="$DURATION" \
  -e TOKENS="${REPO_ROOT}/seed/tokens.json" \
  "${REPO_ROOT}/bench/load.js"

wait "$PPROF_BG_PID" 2>/dev/null || true

# 7. Dừng server sau khi k6 đã hoàn thành và in bảng
echo ""
echo "6. k6 đã chạy xong toàn bộ! Đang dừng server và giải phóng tài nguyên..."
kill -9 "$SERVER_PID" 2>/dev/null || true
pkill -9 -f "go-iouring-quananh2503/bin/server" 2>/dev/null || true
fuser -k 3000/tcp 6060/tcp 2>/dev/null || true
rm -f /tmp/bench_feed.db /tmp/bench_feed.db-wal /tmp/bench_feed.db-shm

# 8. Tóm tắt và Mở giao diện pprof Web UI
echo ""
echo "======================================================================"
echo "🎯 KẾT QUẢ THU THẬP PROFILE LÚC TẢI ĐỈNH:"
echo "  - File Profile:  ${SUBMISSION_DIR}/cpu.prof"
echo "  - File Binary:   ${SUBMISSION_DIR}/bin/server"
echo ""
echo "📊 TOP CÁC HÀM TỐN NHIỀU CPU NHẤT TRONG 20s TẢI ĐỈNH:"
go tool pprof -top -cum "${SUBMISSION_DIR}/bin/server" "${SUBMISSION_DIR}/cpu.prof" | head -n 30
echo ""
echo "🌐 Đang khởi chạy pprof Web UI (Flame Graph) tại port ${PPROF_PORT}..."
fuser -k "${PPROF_PORT}/tcp" 2>/dev/null || true
nohup go tool pprof -http=0.0.0.0:"${PPROF_PORT}" -no_browser "${SUBMISSION_DIR}/bin/server" "${SUBMISSION_DIR}/cpu.prof" > /tmp/pprof_web.log 2>&1 &
sleep 2
echo "✅ pprof Web UI đang chạy tại: http://localhost:${PPROF_PORT}/ui/flamegraph"
echo "======================================================================"
