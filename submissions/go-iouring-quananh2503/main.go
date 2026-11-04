package main

import (
	"log"
	"net/http"
	_ "net/http/pprof"
	"runtime"

	httpserver "go-iouring-quananh2503/src/http-server"
)

func main() {
	runtime.LockOSThread()

	// Mở endpoint pprof trên port 6060 phục vụ profiling đo đạc điểm nghẽn
	go func() {
		log.Println("pprof server đang lắng nghe tại http://127.0.0.1:6060/debug/pprof/")
		if err := http.ListenAndServe("127.0.0.1:6060", nil); err != nil && err != http.ErrServerClosed {
			log.Printf("pprof lỗi: %v", err)
		}
	}()

	cfg := httpserver.LoadConfig()
	srv, err := httpserver.NewServer(cfg)
	if err != nil {
		log.Fatalf("Khởi động server thất bại: %v", err)
	}
	defer srv.Close()

	if err := srv.Run(); err != nil {
		log.Fatalf("Server dừng vì lỗi: %v", err)
	}
}
