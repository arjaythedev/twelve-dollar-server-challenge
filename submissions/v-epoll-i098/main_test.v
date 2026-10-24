module main

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
