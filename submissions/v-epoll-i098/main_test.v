module main

import time

fn test_output_drain_resumes_after_checkpoint_finished() {
	mut s := Server{
		conns: [Conn{open: true, deferred: true}]
	}
	s.output_drained(0)

	assert s.conns[0].ready
	assert !s.conns[0].deferred
	assert s.conns[0].queued
	assert s.flush == [0]
}

fn test_output_drain_waits_for_running_checkpoint() {
	mut s := Server{
		conns: [Conn{open: true, deferred: true}]
		writes_paused: true
	}
	s.output_drained(0)

	assert s.conns[0].ready
	assert s.conns[0].deferred
	assert !s.conns[0].queued
	assert s.flush.len == 0
}

fn test_checkpoint_holds_writes_when_frames_arrived_during_phase_a() {
	mut s := Server{
		ckpt_efd: C.eventfd(0, C.EFD_NONBLOCK | C.EFD_CLOEXEC)
		ckpt_phase: 1
		ckpt_frames_at_start: 1000
		wal_frames: 1200
	}
	complete := u64(1)
	C.write(s.ckpt_efd, &complete, 8)
	s.checkpoint_done()

	assert s.ckpt_phase == 2
	assert s.writes_paused
}

fn test_commit_that_wrote_holds_later_writes_for_the_window() {
	mut s := Server{
		wal_written: true
	}
	before := time.sys_mono_now()
	s.finish_batch()

	assert s.writes_paused
	assert s.pause_until >= before + write_window_ns
	assert !s.wal_written
}

fn test_read_only_batch_leaves_writes_open() {
	mut s := Server{}
	s.finish_batch()

	assert !s.writes_paused
}
