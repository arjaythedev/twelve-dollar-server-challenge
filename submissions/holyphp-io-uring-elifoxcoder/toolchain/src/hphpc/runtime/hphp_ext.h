/* hphp_ext.h — builtin prototypes for the optional runtime modules.
 *
 * hphp_rt.h is the core ABI. The transport (hphp_uring.c), SQLite
 * (hphp_sqlite.c) and the generic additions in hphp_std.c add more
 * builtins, and generated code needs their prototypes even though the
 * modules are only linked when a program uses them. The compiler passes
 * this header with -include, so a program never has to import anything.
 */
#ifndef HPHP_EXT_H
#define HPHP_EXT_H
/* forced first by the backend: keep the libc feature macros ahead of
 * every system header the program or a runtime module includes. */
#if !defined(_WIN32)
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1
#endif
#include "hphp_rt.h"

hval hpbi_accept_emfile(void);
hval hpbi_arr_clear(hval a);
hval hpbi_arr_free(hval a);
hval hpbi_bt_close(hval h);
hval hpbi_bt_col_int(hval i);
hval hpbi_bt_col_int_at(hval h, hval col);
hval hpbi_bt_col_len(hval i);
hval hpbi_bt_col_off(hval i);
hval hpbi_bt_cols(hval h);
hval hpbi_bt_count_key(hval h, hval key);
hval hpbi_bt_count_range(hval h, hval lo, hval hi, hval out);
hval hpbi_bt_first(hval h);
hval hpbi_bt_last(hval h);
hval hpbi_bt_multi_keys(hval result, hval out);
hval hpbi_bt_multi_payloads(hval result, hval out);
hval hpbi_bt_multi_release(hval result);
hval hpbi_bt_multi_seek(hval h, hval keys, hval count);
hval hpbi_bt_next(hval h);
hval hpbi_bt_open(hval dbv, hval name, hval isidx);
hval hpbi_bt_open_safe(hval dbv, hval name, hval isidx);
hval hpbi_bt_page_release(hval pin);
hval hpbi_bt_payload_pin(hval h);
hval hpbi_bt_payload_ptr(hval h);
hval hpbi_bt_payload_size(hval h);
hval hpbi_bt_prev(hval h);
hval hpbi_bt_read(hval h, hval out, hval cols);
hval hpbi_bt_root(hval dbv, hval name, hval isidx);
hval hpbi_bt_rowid(hval h);
hval hpbi_bt_seek(hval h, hval rowid);
hval hpbi_bt_tail(hval h, hval count);
hval hpbi_epoll_add(hval s);
hval hpbi_epoll_del(hval s);
hval hpbi_epoll_mod(hval s, hval mask);
hval hpbi_epoll_wait2(hval inv, hval outv, hval timeout_ms);
hval hpbi_epoll_wait_iarr2(hval inv, hval outv, hval timeout_ms);
hval hpbi_epoll_wait_iarr2_safe(hval inv, hval outv, hval timeout_ms);
hval hpbi_epoll_wait_into(hval outv, hval timeout_ms);
hval hpbi_ffi_call(hval fn, hval args);
hval hpbi_ffi_load(hval lib);
hval hpbi_heap_dump(hval p);
hval hpbi_exec_call(hval cmd, ...);
/* exec() is declared with two C parameters in hphp_rt.h but the language
 * contract is one argument; route both forms to the variadic entry point. */
#define hpbi_exec(...) hpbi_exec_call(__VA_ARGS__)
hval hpbi_http_request_bounds(hval s);
hval hpbi_i64_at(hval p, hval off);
hval hpbi_i64_set(hval p, hval off, hval v);
hval hpbi_iarr_data_ptr(hval h);
hval hpbi_iarr_get(hval h, hval i);
hval hpbi_iarr_get_fast(hval h, hval i);
hval hpbi_iarr_len(hval h);
hval hpbi_iarr_new(hval size);
hval hpbi_iarr_new_uninit(hval size);
hval hpbi_iarr_release(hval h);
hval hpbi_iarr_set(hval h, hval i, hval v);
hval hpbi_iarr_set_fast(hval h, hval i, hval v);
hval hpbi_input(hval p);
hval hpbi_int_digits(hval n);
hval hpbi_json_num_field(hval j, hval k);
hval hpbi_json_str_field(hval j, hval k);
hval hpbi_jwt_verifier_new(hval secret);
hval hpbi_jwt_verify(hval tok, hval secret, hval now);
hval hpbi_jwt_verify_prepared(hval tok, hval prepared, hval now);
hval hpbi_live_allocs(void);
hval hpbi_live_bytes(void);
hval hpbi_lcg_value(void);
hval hpbi_nofile_limit(void);
hval hpbi_object_free(hval obj);
hval hpbi_runtime_diag_write(hval message);
hval hpbi_runtime_stats(void);
hval hpbi_sb_bytes(hval h, hval ptr, hval len);
hval hpbi_sb_data_ptr(hval h);
hval hpbi_sb_discard(hval h);
hval hpbi_sb_int(hval h, hval n);
hval hpbi_sb_json(hval h, hval ptr, hval len);
hval hpbi_sb_json_str(hval h, hval str);
hval hpbi_sb_len(hval h);
hval hpbi_sb_new(void);
hval hpbi_sb_prepend(hval h, hval str);
hval hpbi_sb_put(hval h, hval str);
hval hpbi_sb_reserve(hval h, hval n);
hval hpbi_sb_take(hval h);
hval hpbi_sb_take_safe(hval h);
hval hpbi_scalar_binop(hval a, hval b);
hval hpbi_scope_begin(void);
hval hpbi_scope_end(void);
hval hpbi_sqlite_bg_checkpoint_active(hval dbv);
hval hpbi_sqlite_bg_checkpoint_hold(hval dbv);
hval hpbi_sqlite_bg_checkpoint_start(hval dbv, hval pathv, hval framesv, hval holdv);
hval hpbi_sqlite_bg_checkpoint_tick(hval dbv);
hval hpbi_sqlite_bind_int(hval stv, hval idx, hval v);
hval hpbi_sqlite_bind_text(hval stv, hval idx, hval v);
hval hpbi_sqlite_busy_timeout(hval dbv, hval ms);
hval hpbi_sqlite_changes(hval dbv);
hval hpbi_sqlite_clear_bindings(hval stv);
hval hpbi_sqlite_close(hval dbv);
hval hpbi_sqlite_column_int(hval stv, hval col);
hval hpbi_sqlite_column_text(hval stv, hval col);
hval hpbi_sqlite_errmsg(hval dbv);
hval hpbi_sqlite_exec(hval dbv, hval sql);
hval hpbi_sqlite_extended_errcode(hval dbv);
hval hpbi_sqlite_finalize(hval stv);
hval hpbi_sqlite_is_lock_error(hval dbv);
hval hpbi_sqlite_last_insert_rowid(hval dbv);
hval hpbi_sqlite_open(hval path);
hval hpbi_sqlite_open_safe(hval path);
hval hpbi_sqlite_open_vfs(hval path, hval vfs);
hval hpbi_sqlite_prepare(hval dbv, hval sql);
hval hpbi_sqlite_prepare_safe(hval dbv, hval sql);
hval hpbi_str_u8_at(hval s, hval off);
hval hpbi_str_i64_at(hval s, hval off);
hval hpbi_sqlite_reset(hval stv);
hval hpbi_sqlite_status(hval dbv);
hval hpbi_sqlite_step(hval stv);
hval hpbi_str_at_eq(hval s, hval start, hval len, hval lit);
hval hpbi_str_find_range(hval s, hval needle, hval start);
hval hpbi_str_free(hval s);
hval hpbi_str_from_bytes(hval ptr, hval len);
hval hpbi_str_int_range(hval s, hval a, hval b);
hval hpbi_str_persist(hval s);
hval hpbi_str_pos_ci(hval h, hval n, hval off);
hval hpbi_str_prefix_ci(hval s, hval p);
hval hpbi_str_to_int(hval s);
hval hpbi_str_to_int_range(hval s, hval start, hval end);
hval hpbi_str_trim_range(hval s, hval a, hval b);
hval hpbi_stream_ready_into(hval outv, hval handles, hval timeout_ms);
hval hpbi_stream_recv_release(hval s);
hval hpbi_stream_send2(hval s, hval a, hval b);
hval hpbi_stream_send2_nb(hval s, hval a, hval b);
hval hpbi_stream_send_nb(hval s, hval data);
hval hpbi_stream_send_sb(hval s, hval h);
hval hpbi_stream_set_nodelay(hval s);
hval hpbi_stream_set_socket_buffers(hval s, hval recv_bytes, hval send_bytes);
hval hpbi_stream_socket_server_reuseport(hval addr);
hval hpbi_u8_at(hval p, hval off);
hval hpbi_u8_set(hval p, hval off, hval v);
hval hpbi_uring_active(void);
hval hpbi_uring_direct(void);
hval hpbi_uring_direct_slots(void);

#endif
