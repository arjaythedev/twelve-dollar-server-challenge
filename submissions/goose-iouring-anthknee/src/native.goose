// Native calls borrow these slices only for the duration of the call.
namespace native;

struct Cqe { ud: u64, result: i32, flags: u32 }
struct FeedRow { id: i64, likes: i64, body_len: i64, created_len: i64, author_len: i64 }

extern fn ring_add(slots: i32) -> i32 = "io_ring_add";
extern fn enter(ring: i32, wait: bool) -> i32 = "io_enter";
extern fn submit_pending() -> i32 = "io_submit_pending";
extern fn reap(ring: i32, out: Cqe[>..]&) = "io_reap";
extern fn accept(ring: i32, listener: i32, ud: u64) -> i32 = "io_accept";
extern fn cancel_accept(ring: i32, accept_ud: u64, ud: u64) -> i32 = "io_cancel_accept";
extern fn recv(ring: i32, slot: i32, ud: u64) -> i32 = "io_recv";
extern fn read(ring: i32, slot: i32, ud: u64) -> i32 = "io_read";
extern fn send(ring: i32, slot: i32, box: i32, offset: u32, ud: u64) -> i32 = "io_send";
extern fn close(ring: i32, slot: i32, shut_ud: u64, close_ud: u64) -> i32 = "io_close";
extern fn take(ring: i32, buffer: i32, length: i32, out: u8[>..]&) = "io_take";
extern fn return_buffer(ring: i32, buffer: i32) = "io_return";
extern fn publish() = "io_publish";
extern fn input_reserve(capacity: i64) -> bool = "io_input_reserve";
extern fn input_new(capacity: i64) -> i32 = "io_input_new";
extern fn input_drop(box: i32) = "io_input_drop";
extern fn input_append(box: i32, bytes: const u8[:]) -> bool = "io_input_append";
extern fn box_new(capacity: i64) -> i32 = "io_box_new";
extern fn box_drop(box: i32) = "io_box_drop";
extern fn box_append(box: i32, bytes: const u8[:]) -> bool = "io_box_append";
extern fn box_append2(box: i32, first: const u8[:], second: const u8[:]) -> bool = "io_box_append2";
extern fn box_len(box: i32) -> u32 = "io_box_len";
extern fn box_cut(box: i32, length: u32) = "io_box_cut";
extern fn box_read(box: i32, out: u8[>..]&) = "io_box_read";
extern fn listen(host: const u8[:], port: i32) -> i32 = "io_listen";
extern fn slots() -> i32 = "io_slots";
extern fn lock_code() = "io_lock_code";
extern fn env(name: const u8[:], out: u8[>..]&) -> bool = "io_env";
extern fn now() -> f64 = "io_now";
extern fn micros() -> i64 = "io_micros";
extern fn log(bytes: const u8[:]) = "io_log";
extern fn stop() -> bool = "io_stop";
extern fn trim_due(now: i64) -> bool = "io_trim_due";
extern fn heap_trim() -> bool = "io_heap_trim";

extern fn open(path: const u8[:], err: u8[>..]&) -> i32 = "sd_open";
extern fn begin() -> i32 = "sd_begin";
extern fn commit() -> bool = "sd_commit";
extern fn seek(tree: i32, key: i64) -> i32 = "sd_seek";
extern fn seek_ge(tree: i32, key: i64) -> i32 = "sd_seek_ge";
extern fn first(tree: i32) -> i32 = "sd_first";
extern fn next(tree: i32) -> i32 = "sd_next";
extern fn row(tree: i32, out: u8[>..]&) -> i64 = "sd_row";
extern fn feed_author_names(ids: const i64[:], offsets: i64[:], out: u8[>..]&) -> bool = "sd_feed_author_names";
extern fn like_counts(lo: i64, hi: i64, counts: u32[:]) -> i32 = "sd_like_counts";
extern fn add_post(uid: i64, text: const u8[:], id: i64&, created: u8[>..]&) -> i32 = "sd_add_post";
extern fn add_like(uid: i64, id: i64) -> i32 = "sd_add_like";
extern fn ping(err: u8[>..]&) -> i32 = "sd_ping";
extern fn feed_step(row: FeedRow&, text: u8[>..]&) -> i32 = "sd_feed_step";
extern fn checkpoint_request() = "sd_checkpoint_request";
extern fn checkpoint_interrupt() = "sd_checkpoint_interrupt";
extern fn checkpoint_poll() -> i32 = "sd_checkpoint_poll";
extern fn wal_frames() -> i64 = "sd_wal_frames";
extern fn wal_reset() = "sd_wal_reset";
extern fn shutdown() = "sd_shutdown";
