//go:build linux && (amd64 || arm64)

// Package netring implements only the Linux 6.8 io_uring network subset used by
// this experiment. All methods, including Close, require the same locked OS
// thread. No SQPOLL, zero-copy sends or post-6.8 features.
package netring

const (
	AcceptTag         uint64 = 1
	opPollAdd                = 6
	opAccept                 = 13
	opCancel                 = 14
	opSend                   = 26
	opRecv                   = 27
	opClose                  = 19
	opShutdown               = 34
	fileIndexAlloc    uint32 = ^uint32(0)
	fixedFileFlag            = 1
	setupCQSize              = 1 << 3
	setupCoop                = 1 << 8
	setupSingleIssuer        = 1 << 12
	setupDefer               = 1 << 13
	featSingleMmap           = 1
	featNodrop               = 1 << 1
	featFastPoll             = 1 << 5
	featExtArg               = 1 << 8
	sqOverflow               = 1 << 1
	cqeBuffer                = 1
	cqeMore                  = 1 << 1
	bufferSize               = 8192
)

// Layouts transcribed from the MIT-licensed Linux v6.8 UAPI (vendored reference).
// All pointers are numeric addresses of mmap memory, never Go heap pointers.
type sqe struct {
	Opcode, Flags      uint8
	Priority           uint16
	FD                 int32
	Off, Addr          uint64
	Len, OpFlags       uint32
	Tag                uint64
	Group, Personality uint16
	FileIndex          uint32
	Addr3, Pad         uint64
}
type cqe struct {
	Tag    uint64
	Result int32
	Flags  uint32
}
type sqOffsets struct {
	Head, Tail, Mask, Entries, Flags, Dropped, Array, Reserved uint32
	UserAddr                                                   uint64
}
type cqOffsets struct {
	Head, Tail, Mask, Entries, Overflow, CQEs, Flags, Reserved uint32
	UserAddr                                                   uint64
}
type params struct {
	SQEntries, CQEntries, Flags, CPU, Idle, Features, WQ uint32
	Reserved                                             [3]uint32
	SQ                                                   sqOffsets
	CQ                                                   cqOffsets
}
type bufReg struct {
	Addr         uint64
	Entries      uint32
	Group, Flags uint16
	Reserved     [3]uint64
}
type enterArg struct {
	Sigmask          uint64
	SigmaskSize, Pad uint32
	TS               uint64
}
type timespec struct{ Sec, Nsec int64 }
type syncCancel struct {
	Addr     uint64
	FD       int32
	Flags    uint32
	Timeout  timespec
	Opcode   uint8
	Pad      [7]byte
	Reserved [3]uint64
}
type rsrcRegister struct {
	NR, Flags  uint32
	Reserved   uint64
	Data, Tags uint64
}
type fileIndexRange struct {
	Offset, Length uint32
	Reserved       uint64
}

// Completion is copied out before CQ head publication. Its tag is a caller's
// generation+fd+operation identity; no pointer is ever encoded in user_data.
type Completion struct {
	Tag    uint64
	Result int32
	Flags  uint32
}

func (c Completion) More() bool      { return c.Flags&cqeMore != 0 }
func (c Completion) HasBuffer() bool { return c.Flags&cqeBuffer != 0 }
