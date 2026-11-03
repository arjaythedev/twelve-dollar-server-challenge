package httpserver

import (
	"fmt"
	"time"

	"go-iouring-quananh2503/src/sqlite"
)

type DB struct {
	StartTime int64
}

func OpenDB(path string) (*DB, error) {
	if err := sqlite.InitFeedEngine(path); err != nil {
		return nil, fmt.Errorf("InitFeedEngine thất bại: %w", err)
	}

	return &DB{
		StartTime: time.Now().Unix(),
	}, nil
}

func (d *DB) Close() error {
	sqlite.CloseFeedEngine()
	return nil
}
