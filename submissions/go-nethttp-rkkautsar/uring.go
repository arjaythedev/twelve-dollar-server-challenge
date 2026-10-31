// Optional batched socket writes through io_uring. One writer goroutine owns
// the ring; connection goroutines submit complete response packets and wait for
// their send completion. This is only used for the prebuilt read packets.
package main

import (
	"errors"
	"net"
	"sync"
	"syscall"

	"github.com/iceber/iouring-go"
)

const uringBatch = 64

type uringJob struct {
	fd   int
	buf  []byte
	off  int
	conn net.Conn
	done chan error
}

type uringWriter struct {
	ring *iouring.IOURing
	jobs chan *uringJob
	once sync.Once
}

func newUringWriter() (*uringWriter, error) {
	ring, err := iouring.New(256)
	if err != nil {
		return nil, err
	}
	w := &uringWriter{ring: ring, jobs: make(chan *uringJob, 1024)}
	go w.loop()
	return w, nil
}

func (w *uringWriter) close() { w.once.Do(func() { _ = w.ring.Close() }) }

func (w *uringWriter) write(fd int, buf []byte, conn net.Conn) error {
	j := &uringJob{fd: fd, buf: buf, conn: conn, done: make(chan error, 1)}
	w.jobs <- j
	return <-j.done
}

func (w *uringWriter) loop() {
	for first := range w.jobs {
		batch := make([]*uringJob, 1, uringBatch)
		batch[0] = first
		for len(batch) < uringBatch {
			select {
			case j := <-w.jobs:
				batch = append(batch, j)
			default:
				goto submit
			}
		}
	submit:
		w.submit(batch)
	}
}

func (w *uringWriter) submit(batch []*uringJob) {
	for len(batch) > 0 {
		ch := make(chan iouring.Result, len(batch))
		reqs := make([]iouring.PrepRequest, len(batch))
		for i, j := range batch {
			reqs[i] = iouring.Send(j.fd, j.buf[j.off:], syscall.MSG_NOSIGNAL).WithInfo(j)
		}
		_, err := w.ring.SubmitRequests(reqs, ch)
		if err != nil {
			for _, j := range batch {
				j.done <- err
			}
			return
		}
		next := batch[:0]
		for range batch {
			res := <-ch
			j := res.GetRequestInfo().(*uringJob)
			n, err := res.ReturnInt()
			if err != nil {
				if j.off == 0 && errors.Is(err, syscall.EAGAIN) {
					// Let Go's netpoll finish this connection's write.
					_, werr := j.conn.Write(j.buf)
					j.done <- werr
					continue
				}
				j.done <- err
				continue
			}
			j.off += n
			if j.off < len(j.buf) {
				next = append(next, j)
			} else {
				j.done <- nil
			}
		}
		batch = next
	}
}
