//go:build linux

package main

import (
	"log"
	"runtime"
)

func serve(host, port, secret string) error {
	runtime.GOMAXPROCS(1)
	runtime.LockOSThread()
	s, err := newServer(host, port, secret)
	if err != nil {
		return err
	}
	log.Printf("listening on %s:%s (%s)", host, port, s.io.name())
	s.io.run()
	return nil
}
