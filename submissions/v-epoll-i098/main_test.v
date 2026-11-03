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

fn body_of(raw string) ?string {
	mut key := []u8{}
	mut body := []u8{}
	return body_member(raw, mut key, mut body)
}

fn test_body_member_rejects_unclosed_array_ending_in_number() {
	// x.json2 never returned on these, which stopped the event loop.
	for raw in ['[0', '[1,0', '[1.5', '{"body":"x","a":[0'] {
		assert body_of(raw) == none
	}
}

fn test_body_member_decodes_body_string() {
	assert body_of('{"body":"hi"}')? == 'hi'
	assert body_of(' { "n" : [1, {"b": null}], "body" : "a\\"b\\u00e9\\ud83d\\ude00" } ')? == 'a"bé😀'
	assert body_of('{"body":"x","body":"y"}')? == 'y'
	assert body_of('{"body":"x","body":1}')? == ''
	assert body_of('[1,2]')? == ''
}

fn test_body_member_rejects_invalid_json() {
	for raw in ['', '{', '{"body":"x",', '{"body":"x"}}', '{"body":"x",}', '{"body":"\\q"}', '{"body":"\\ud800"}',
		'{"body":"x","n":01}', '{"body":"x","n":1.}'] {
		assert body_of(raw) == none
	}
}

fn test_put_int_matches_string_conversion() {
	for v in [i64(0), 9, 10, 99, 100, 101, 12345, 592594, -1, -10, -4096, 9223372036854775807,
		-9223372036854775807] {
		mut b := []u8{}
		put_int(mut b, v)
		assert b.bytestr() == v.str()
	}
}

fn test_put_json_escapes_at_every_position_and_length() {
	for n in 0 .. 70 {
		for ch in [u8(`"`), `\\`, `\n`, 0x01, 0x1f, 0x7f, 0xc3] {
			for pos in 0 .. n {
				mut src := []u8{len: n, init: `a`}
				src[pos] = ch
				mut got := []u8{}
				put_json(mut got, src.data, n)
				mut want := [u8(`"`)]
				for c in src {
					match c {
						`"` { want << [u8(`\\`), `"`] }
						`\\` { want << [u8(`\\`), `\\`] }
						`\n` { want << [u8(`\\`), `n`] }
						0x01, 0x1f { want << '\\u00${hex[c >> 4]:c}${hex[c & 15]:c}'.bytes() }
						else { want << c }
					}
				}
				want << `"`
				assert got == want
			}
		}
	}
}
