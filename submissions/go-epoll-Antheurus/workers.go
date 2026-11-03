package main

// Workers exist for file descriptors, not CPU. LimitNOFILE=65535 is per process, so one process
// holds ~59k sockets; past that it must close idle keep-alive connections and k6 reconnects.
// More processes hold more sockets, but every process that serves traffic costs CPU of its own
// (more wakeups, smaller batches per wakeup); the README has the measurements.
//
// So the kernel is told where new connections go, and only one worker fills at a time. All bind
// the port with SO_REUSEPORT, and worker 0 attaches a one-instruction classic BPF program to the
// group (SO_ATTACH_REUSEPORT_CBPF) that returns the index of the worker currently filling up.
// When that worker nears its fd budget it says so over a pipe, and worker 0 re-points the program
// at the next worker; an idle worker only wakes for its housekeeping tick. Nothing is ever
// closed, so no connection waiting in an accept queue is lost. socket(7): sockets are numbered in
// listen() order, and an out-of-range index falls back to plain hashing. If a worker dies,
// worker 0 detaches the program (plain hashing over the survivors, still correct) and restarts it.

import (
	"errors"
	"log"
	"os"
	"os/exec"
	"strconv"
	"syscall"
	"unsafe"
)

const (
	maxWorkers = 3
	workerEnv  = "TQ_WORKER"

	soAttachReuseportCBPF = 51
	soDetachReuseportBPF  = 68

	msgReady = 'R'
	msgFull  = 'F'
)

// steer points new connections on the port at the socket with the given index.
func steer(lfd, index int) error {
	prog := [1]syscall.SockFilter{{Code: syscall.BPF_RET | syscall.BPF_K, K: uint32(index)}}
	fprog := syscall.SockFprog{Len: 1, Filter: &prog[0]}
	_, _, e := syscall.Syscall6(syscall.SYS_SETSOCKOPT, uintptr(lfd), syscall.SOL_SOCKET, soAttachReuseportCBPF,
		uintptr(unsafe.Pointer(&fprog)), unsafe.Sizeof(fprog), 0)
	if e != 0 {
		return e
	}
	return nil
}

func unsteer(lfd int) {
	var zero int32
	syscall.Syscall6(syscall.SYS_SETSOCKOPT, uintptr(lfd), syscall.SOL_SOCKET, soDetachReuseportBPF,
		uintptr(unsafe.Pointer(&zero)), unsafe.Sizeof(zero), 0)
}

// controller runs in worker 0's event loop. Every worker is started at boot, one after another
// so that listen() order (the index the program returns) is known; starting one later would need
// fresh file descriptors at exactly the moment the starting worker has none left.
type controller struct {
	s        *server
	self     string
	steering bool
	active   int // worker receiving new connections
	ready    [maxWorkers]bool
	pipes    [maxWorkers]int // read end of each child's report pipe, -1 when none
	pending  [maxWorkers]bool
	deathR   int
	deathW   *os.File
	buf      [64]byte
}

func newController(s *server) (*controller, error) {
	self, err := os.Executable()
	if err != nil {
		return nil, err
	}
	r, w, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	c := &controller{s: s, self: self, deathR: int(r.Fd()), deathW: w}
	c.ready[0] = true
	for i := range c.pipes {
		c.pipes[i] = -1
	}
	syscall.SetNonblock(c.deathR, true)
	s.watch(c.deathR)
	if err := steer(s.lfd, 0); err != nil {
		log.Printf("reuseport steering unavailable (%v); connections spread by hash", err)
	} else {
		c.steering = true
	}
	if maxWorkers > 1 && !c.spawn(1) {
		return nil, errSpawn
	}
	return c, nil
}

var errSpawn = errors.New("could not start worker 1")

func (c *controller) owns(fd int) bool {
	if fd == c.deathR {
		return true
	}
	for _, p := range c.pipes {
		if p == fd {
			return true
		}
	}
	return false
}

func (c *controller) handle(fd int) {
	n, _, e := syscall.RawSyscall(syscall.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(&c.buf[0])), uintptr(len(c.buf)))
	if e != 0 || int(n) <= 0 {
		return
	}
	for _, b := range c.buf[:n] {
		if fd == c.deathR {
			c.died(int(b))
			continue
		}
		i := c.workerOf(fd)
		switch b {
		case msgReady:
			c.ready[i] = true
			if i+1 < maxWorkers && c.pipes[i+1] < 0 {
				c.spawn(i + 1)
			}
		case msgFull:
			if i == c.active {
				c.advance()
			}
		}
	}
}

func (c *controller) workerOf(fd int) int {
	for i, p := range c.pipes {
		if p == fd {
			return i
		}
	}
	return -1
}

// tick retries, on worker 0's housekeeping tick, a worker that could not be restarted.
func (c *controller) tick() {
	for i, p := range c.pending {
		if p && c.spawn(i) {
			c.pending[i] = false
		}
	}
}

// advance points new connections at the next worker; once there is none, it falls back to hashing.
func (c *controller) advance() {
	if !c.steering {
		return
	}
	next := c.active + 1
	if next < maxWorkers && c.ready[next] {
		if err := steer(c.s.lfd, next); err == nil {
			c.active = next
			log.Printf("new connections now go to worker %d", next)
			return
		}
	}
	unsteer(c.s.lfd)
	c.steering = false
	log.Printf("no idle worker left to fill; connections spread by hash")
}

func (c *controller) spawn(i int) bool {
	r, w, err := os.Pipe()
	if err != nil {
		log.Printf("worker %d: %v", i, err)
		return false
	}
	cmd := exec.Command(c.self)
	cmd.Env = append(os.Environ(), workerEnv+"="+strconv.Itoa(i))
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	cmd.ExtraFiles = []*os.File{w}
	// Pdeathsig follows the thread that forked, and this is the event loop's thread, which is
	// locked and never exits: children die with worker 0.
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	if err := cmd.Start(); err != nil {
		log.Printf("worker %d: %v", i, err)
		r.Close()
		w.Close()
		return false
	}
	w.Close()
	fd, err := syscall.Dup(int(r.Fd()))
	r.Close()
	if err != nil {
		log.Printf("worker %d pipe: %v", i, err)
		cmd.Process.Kill()
		return false
	}
	syscall.SetNonblock(fd, true)
	c.pipes[i] = fd
	c.s.watch(fd)
	go func() {
		err := cmd.Wait()
		log.Printf("worker %d exited (%v)", i, err)
		c.deathW.Write([]byte{byte(i)})
	}()
	return true
}

// died replaces a dead worker. Its socket left the group and the kernel moved the last socket
// into its slot, so the program's indices no longer name the right workers: fall back to hashing.
func (c *controller) died(i int) {
	if c.steering {
		unsteer(c.s.lfd)
		c.steering = false
		log.Printf("worker %d died; connections spread by hash from now on", i)
	}
	c.ready[i] = false
	if p := c.pipes[i]; p >= 0 {
		c.s.unwatch(p)
		syscall.Close(p)
		c.pipes[i] = -1
	}
	if !c.spawn(i) {
		c.pending[i] = true
	}
}

// reporter is a child worker's side of the pipe.
type reporter struct {
	f    *os.File
	full bool
}

func (r *reporter) send(b byte) {
	r.f.Write([]byte{b})
}
