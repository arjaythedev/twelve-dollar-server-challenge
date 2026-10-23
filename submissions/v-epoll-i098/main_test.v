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
