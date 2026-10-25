package main

// The socket operations, as SQE preparers.
//
// Three io_uring features carry this design, and all three show up here:
//
//   Direct descriptors. An accepted connection is placed into the ring's registered file table
//   and referred to by index. It never becomes a file descriptor, so the process's descriptor
//   count does not grow with the connection count and RLIMIT_NOFILE stops being the ceiling.
//
//   Multishot. One accept SQE yields a completion per connection, and one receive SQE yields a
//   completion per arriving message, until the kernel says otherwise. Submissions per request
//   fall to roughly one send.
//
//   Provided buffers. A multishot receive takes its buffer from a group at the moment data
//   arrives, so an idle connection holds none.

import "unsafe"

// file_index is stored as index+1, with 0 meaning "not a fixed file". IORING_FILE_INDEX_ALLOC
// asks the kernel to pick a free slot and report it in cqe.res. The kernel field is a __u32 and
// Go's is an int32, and the value is (~0U), so it is written as -1: the same 32 bits.
const fileIndexAlloc int32 = -1

// ioprio values that turn an operation multishot.
const (
	acceptMultishot = 1 << 0 // IORING_ACCEPT_MULTISHOT
	recvMultishot   = 1 << 1 // IORING_RECV_MULTISHOT
)

// IORING_OP_ASYNC_CANCEL flags.
const (
	cancelAll     = 1 << 0 // cancel every match, not just the first
	cancelFD      = 1 << 1 // match on the fd rather than on user_data
	cancelAny     = 1 << 2
	cancelFDFixed = 1 << 3 // the fd to match is a registered-table index
)

const (
	msgNoSignal = 0x4000 // MSG_NOSIGNAL: a send to a closed peer must not raise SIGPIPE
	msgWaitAll  = 0x100
)

// setTargetFixedFile points an operation's *result* at a registered file slot.
func (s *sqe) setTargetFixedFile(index uint32) {
	s.spliceFDIn = int32(index + 1)
}

// setTargetFixedFileAlloc asks the kernel to allocate any free slot.
func (s *sqe) setTargetFixedFileAlloc() {
	s.spliceFDIn = fileIndexAlloc
}

// prepAcceptMultishotDirect arms a multishot accept on a listening socket, placing each accepted
// connection into a free slot of this ring's registered file table.
//
// It stays armed and posts one completion per connection, each with cqeFMore set. cqe.res is the
// table index. When cqeFMore is absent the accept has terminated and must be re-armed, or, once
// the table is full, handed to another ring: a full table terminates the accept with ENFILE or
// EMFILE, which is exactly the signal the multi-ring handoff is driven by.
//
// No sockaddr is requested. The peer address costs a copy per connection and this server never
// looks at it.
func (s *sqe) prepAcceptMultishotDirect(listenFD int32, flags uint32, userData uint64) {
	s.opcode = opAccept
	s.fd = listenFD
	s.addr = 0 // no sockaddr out
	s.off = 0  // no addrlen out
	s.opFlags = flags
	s.ioprio = acceptMultishot
	s.userData = userData
	s.setTargetFixedFileAlloc()
}

// prepRecvMultishot arms a multishot receive on a registered connection, taking buffers from
// group bgid as data arrives.
//
// Each completion carries the buffer id in the upper 16 bits of cqe.flags and the byte count in
// cqe.res. res == 0 means the peer closed. ENOBUFS means the group ran dry, which also terminates
// the multishot, so the caller must re-arm after refilling.
func (s *sqe) prepRecvMultishot(fileIndex uint32, bgid uint16, userData uint64) {
	s.opcode = opRecv
	s.fd = int32(fileIndex)
	s.addr = 0 // the buffer comes from the group
	s.length = 0
	s.opFlags = 0
	s.ioprio = recvMultishot
	s.bufIndex = bgid
	s.userData = userData
	s.flags = sqeFixedFile | sqeBufferSelect
}

// prepSend queues a send on a registered connection. The buffer must be off-heap: the kernel
// reads it after io_uring_enter returns.
func (s *sqe) prepSend(fileIndex uint32, buf []byte, userData uint64) {
	s.opcode = opSend
	s.fd = int32(fileIndex)
	s.addr = uint64(uintptr(unsafe.Pointer(unsafe.SliceData(buf))))
	s.length = uint32(len(buf))
	s.opFlags = msgNoSignal
	s.userData = userData
	s.flags = sqeFixedFile
}

// prepCloseDirect releases a registered file slot, closing the socket in it.
func (s *sqe) prepCloseDirect(fileIndex uint32, userData uint64) {
	s.opcode = opClose
	s.fd = 0
	s.userData = userData
	s.setTargetFixedFile(fileIndex)
}

// prepCancelFixedFile cancels every pending operation on a registered connection.
//
// This is the first half of the close sequence. A pending multishot receive holds its own
// reference to the socket, so closing the slot underneath it is not enough: the operation has to
// be cancelled first. Link this to the close with sqeIOHardlink so the close runs even when the
// cancel finds nothing, which happens whenever the receive has already completed.
func (s *sqe) prepCancelFixedFile(fileIndex uint32, userData uint64) {
	s.opcode = opAsyncCancel
	s.fd = int32(fileIndex)
	s.addr = 0
	s.opFlags = cancelFD | cancelFDFixed | cancelAll
	s.userData = userData
}

// prepCancelUserData cancels the operation carrying a given user_data. It is how a multishot
// accept is moved off a ring before its registered file table fills.
func (s *sqe) prepCancelUserData(target, userData uint64) {
	s.opcode = opAsyncCancel
	s.fd = -1
	s.addr = target
	s.opFlags = 0
	s.userData = userData
}

// Poll flags, carried in sqe->len.
const (
	pollAddMulti = 1 << 0 // IORING_POLL_ADD_MULTI: stay armed after each event
	pollAddLevel = 1 << 1 // IORING_POLL_ADD_LEVEL: level triggered, not edge
)

// prepPollAdd watches a descriptor with explicit poll flags.
func (s *sqe) prepPollAdd(fd int32, events, pollFlags uint32, userData uint64) {
	s.opcode = opPollAdd
	s.fd = fd
	s.length = pollFlags
	s.opFlags = events
	s.userData = userData
}

// prepNopUserData posts a completion without doing any work, used to wake the loop.
func (s *sqe) prepNopUserData(userData uint64) { s.prepNop(userData) }
