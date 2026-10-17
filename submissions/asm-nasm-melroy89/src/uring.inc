; Diagnostic-only counters compile away completely in ordinary builds.
%macro IO_INC 1
%ifdef IO_DIAG
 inc qword [io_counters+8*%1]
%endif
%endmacro
%macro IO_HIGH 2
%ifdef IO_DIAG
 push rax
 mov rax,[%1]
 cmp rax,[io_counters+8*%2]
 jbe %%done
 mov [io_counters+8*%2],rax
%%done:
 pop rax
%endif
%endmacro
%ifdef IO_DIAG
section .rodata
io_prefix: db 'IO_STATS {"counters":[',0
section .bss
io_counters: resq 13
io_last_sec: resq 1
io_output: resb 1024
section .text
io_dump:
 ENTER
 mov rax,[now_sec]
 cmp rax,[io_last_sec]
 je .done
 mov [io_last_sec],rax
 lea rdi,[io_output]
 lea rsi,[io_prefix]
 call append_z
 xor ebx,ebx
.each:
 lea rax,[io_counters]
 mov rax,[rax+rbx*8]
 call append_uint
 mov byte [rdi],','
 inc rdi
 inc ebx
 cmp ebx,13
 jb .each
 mov rax,[request_count]
 call append_uint
 mov byte [rdi],','
 inc rdi
 mov rax,[active_count]
 call append_uint
 mov byte [rdi],','
 inc rdi
 mov rax,[input_allocated]
 call append_uint
 mov byte [rdi],','
 inc rdi
 mov rax,[output_allocated]
 call append_uint
 mov word [rdi],0x7d5d
 mov byte [rdi+2],10
 add rdi,3
 mov rdx,rdi
 lea rsi,[io_output]
 sub rdx,rsi
 mov eax,1
 mov edi,2
 syscall
.done: LEAVE
%endif
; SPDX-License-Identifier: MIT
; Raw io_uring backend. One issuer, multiple sparse fixed-file tables.
; No SQPOLL, no liburing. x86 TSO: plain aligned publications implement release/acquire.
%define RINGS 8
%define RSTRIDE 512
%define SQSIZE 4096
%define NBUFS 1024
%define BSIZE 2048
section .rodata
uring_ready: db 'NASM direct HTTP ready (raw io_uring, fixed sockets, provided buffers).',10
uring_ready_len equ $-uring_ready
section .bss
alignb 64
uring_enabled: resd 1
force_epoll: resd 1
uring_count: resd 1
ring_slots: resd 1
; Bounded generation-tagged retry FIFO: 65,536 entries per ring.
enobuf_queue: resq RINGS*65536
ring_contexts: resb RINGS*RSTRIDE
rlimits: resq 2
sparse_reg: resb 32
buffer_reg: resb 40
uring_waits: resq 1
wait_arg: resb 24
wait_time: resq 2
batch_clock_start: resq 2
batch_clock_now: resq 2
section .text
; map_ring(length rsi, fd r8, offset r9) -> pointer
map_ring:
 mov eax,9
 xor edi,edi
 mov edx,3
 mov r10d,1
 syscall
 ret
uring_init:
 ENTER
 mov eax,97
 mov edi,7
 lea rsi,[rlimits]
 syscall
 test eax,eax
 js .fail
 mov rax,[rlimits]
 cmp rax,65535
 jbe .slots
 mov eax,65535
.slots:
 cmp eax,64
 jb .fail
 mov [ring_slots],eax
 xor ebx,ebx
.each:
 mov r12d,ebx
 shl r12,9
 lea rax,[ring_contexts]
 add r12,rax
 lea rdi,[r12+256]
 xor eax,eax
 mov ecx,15
 rep stosq
 mov dword [r12+264],0x1100 ; SINGLE_ISSUER | COOP_TASKRUN (no DEFER)
 mov eax,425
 mov edi,SQSIZE
 lea rsi,[r12+256]
 syscall
 test eax,eax
 js .partial
 mov [r12],eax
 inc dword [uring_count]
 ; Sparse file registration, size bounded by current RLIMIT_NOFILE.
 mov eax,[ring_slots]
 mov [sparse_reg],eax
 mov dword [sparse_reg+4],1
 mov eax,427
 mov edi,[r12]
 mov esi,13
 lea rdx,[sparse_reg]
 mov r10d,32
 syscall
 test eax,eax
 js .fail
 ; SQ and CQ mappings: use returned offsets, not guessed kernel layout.
 mov esi,[r12+256]
 shl rsi,2
 mov eax,[r12+320] ; params.sq_off.array = 40+24=64
 add rsi,rax
 mov r8d,[r12]
 xor r9d,r9d
 call map_ring
 test rax,rax
 js .fail
 mov [r12+8],rax
 mov esi,[r12+260]
 shl rsi,4
 mov edx,[r12+356] ; cq_off.cqes = 80+20
 add rsi,rdx
 mov r8d,[r12]
 mov r9d,0x8000000
 call map_ring
 test rax,rax
 js .fail
 mov [r12+16],rax
 mov esi,[r12+256]
 shl rsi,6
 mov r8d,[r12]
 mov r9d,0x10000000
 call map_ring
 test rax,rax
 js .fail
 mov [r12+24],rax
 mov rax,[r12+8]
 mov ecx,[r12+296]
 lea rdx,[rax+rcx]
 mov [r12+32],rdx
 mov ecx,[r12+300]
 lea rdx,[rax+rcx]
 mov [r12+40],rdx
 mov ecx,[r12+304]
 lea rdx,[rax+rcx]
 mov [r12+48],rdx
 mov ecx,[r12+320]
 lea rdx,[rax+rcx]
 mov [r12+56],rdx
 mov rax,[r12+16]
 mov ecx,[r12+336]
 lea rdx,[rax+rcx]
 mov [r12+64],rdx
 mov ecx,[r12+340]
 lea rdx,[rax+rcx]
 mov [r12+72],rdx
 mov ecx,[r12+344]
 lea rdx,[rax+rcx]
 mov [r12+80],rdx
 mov ecx,[r12+356]
 lea rdx,[rax+rcx]
 mov [r12+88],rdx
 mov esi,NBUFS*16
 call map_memory
 test rax,rax
 js .fail
 mov [r12+96],rax
 mov [buffer_reg],rax
 mov dword [buffer_reg+8],NBUFS
 mov eax,427
 mov edi,[r12]
 mov esi,22
 lea rdx,[buffer_reg]
 mov r10d,1
 syscall
 test eax,eax
 js .fail
 mov esi,NBUFS*BSIZE
 call map_memory
 test rax,rax
 js .fail
 mov [r12+104],rax
 mov rdi,[r12+96]
 xor ecx,ecx
.buffers:
 mov [rdi],rax
 mov dword [rdi+8],BSIZE
 mov [rdi+12],cx
 add rax,BSIZE
 add rdi,16
 inc ecx
 cmp ecx,NBUFS
 jb .buffers
 mov rax,[r12+96]
 mov word [rax+14],NBUFS
 mov dword [r12+112],NBUFS
 ; Poll all ring FDs in epoll. COOP_TASKRUN avoids deferred-work sleep deadlock.
 mov dword [ctl_event],1
 mov rax,-2
 sub rax,rbx
 mov [ctl_event+4],rax
 mov eax,233
 mov edi,[epoll_fd]
 mov esi,1
 mov edx,[r12]
 lea r10,[ctl_event]
 syscall
 test eax,eax
 js .fail
 inc ebx
 cmp ebx,[requested_rings]
 jb .each
.partial:
 cmp dword [uring_count],0
 je .fail
 ; Any ring registered before a partial failure must be completely initialized.
 cmp ebx,[uring_count]
 jne .fail
 mov dword [uring_enabled],1
 xor edi,edi
 call uring_arm_accept
 xor eax,eax
 LEAVE
.fail:
 ; Fail closed for partial initialization; don't leave hidden rings live in fallback.
 xor ebx,ebx
.cleanup:
 cmp ebx,[uring_count]
 jae .failed
 mov eax,ebx
 shl rax,9
 lea rdx,[ring_contexts]
 mov edi,[rdx+rax]
 mov eax,3
 syscall
 inc ebx
 jmp .cleanup
.failed:
 mov dword [uring_count],0
 mov eax,-1
 LEAVE
; rdi ring index -> cleared SQE rax, publishes at function end (single owner).
uring_sqe:
 ENTER
 mov r12d,edi
 shl r12,9
 lea rax,[ring_contexts]
 add r12,rax
 mov r13,[r12+40]
 mov ebx,[r13]
 mov rax,[r12+32]
 mov eax,[rax]
 mov edx,ebx
 sub edx,eax
 cmp edx,SQSIZE
 jb .space
 mov eax,426
 mov edi,[r12]
 mov esi,edx
 xor edx,edx
 mov r10d,1
 xor r8d,r8d
 xor r9d,r9d
 syscall
 mov rax,[r12+32]
 mov eax,[rax]
 mov edx,ebx
 sub edx,eax
 cmp edx,SQSIZE
 jae fatal
.space:
 mov rax,[r12+48]
 mov eax,[rax]
 and eax,ebx
 mov rdx,[r12+56]
 mov [rdx+rax*4],eax
 shl rax,6
 add rax,[r12+24]
 mov r14,rax
 mov rdi,rax
 xor eax,eax
 mov ecx,8
 rep stosq
 inc ebx
 ; No kernel consumer runs concurrently without enter (SQPOLL is disabled).
 mov [r13],ebx
 mov rax,r14
 LEAVE
uring_arm_accept:
 ENTER
 mov ebx,edi
 mov r12d,edi
 shl r12,9
 lea rax,[ring_contexts]
 add r12,rax
.loop:
 cmp dword [r12+120],64
 jae .done
 mov eax,[r12+116]
 add eax,[r12+120]
 cmp eax,[ring_slots]
 jae .done
 mov edi,ebx
 call uring_sqe
 mov byte [rax],13
 mov edx,[listen_fd]
 mov [rax+4],edx
 mov dword [rax+28],0
 mov dword [rax+44],-1
 mov edx,ebx
 or edx,0x100000
 mov [rax+32],rdx
 inc dword [r12+120]
 jmp .loop
.done: LEAVE
; fd logical index -> SQE with fixed fd and generation-tagged user_data.
uring_connection_sqe:
 ENTER
 mov ebx,edi
 shr edi,16
 call uring_sqe
 mov byte [rax+1],1
 mov edx,ebx
 and edx,65535
 mov [rax+4],edx
 lea rcx,[generations]
 mov ecx,[rcx+rbx*4]
 shl rcx,32
 or rcx,rbx
 mov [rax+32],rcx
 LEAVE
uring_recv:
 ENTER
 mov ebx,edi
 mov r12d,edi
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],16
 jnz .done
 test dword [r12+60],32|256
 jnz .done
 call uring_connection_sqe
 mov byte [rax],27
 or byte [rax+1],32 ; buffer selection
 mov dword [rax+24],BSIZE
 cmp dword [recv_mode],0
 je .oneshot
 mov word [rax+2],2
 mov dword [rax+24],0
.oneshot:
 or qword [rax+32],0x200000
 or dword [r12+60],32
.done: LEAVE
uring_send:
 ENTER
 mov ebx,edi
 mov r12d,edi
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],64
 jnz .done
 call uring_connection_sqe
 mov byte [rax],26
 mov rcx,[r12+16]
 add rcx,[r12+32]
 mov [rax+16],rcx
 mov rcx,[r12+24]
 sub rcx,[r12+32]
 mov [rax+24],ecx
 mov dword [rax+28],0x4000
 or qword [rax+32],0x300000
 or dword [r12+60],64
.done: LEAVE
uring_close:
 ENTER
 mov ebx,edi
 mov r12d,edi
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],1
 jz .done
 test dword [r12+60],16
 jnz .done
 call timer_remove
 or dword [r12+60],16
 ; Cancel outstanding socket operations, then remove direct descriptor.
 mov edi,ebx
 call uring_connection_sqe
 mov byte [rax],14
 mov byte [rax+1],0
 mov dword [rax+28],11 ; FD | ALL | FD_FIXED
 or qword [rax+32],0x500000
 mov edi,ebx
 call uring_connection_sqe
 mov byte [rax],19
 mov byte [rax+1],0
 mov dword [rax+4],0
 mov ecx,ebx
 and ecx,65535
 inc ecx
 mov [rax+44],ecx
 or qword [rax+32],0x400000
.done: LEAVE
; Release only after close completion and all send/receive references are gone.
uring_retire:
 ENTER
 mov ebx,edi
 mov r12d,edi
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],128
 jz .done
 test dword [r12+60],96
 jnz .done
 mov rdi,[r12]
 test rdi,rdi
 jz .out
 call release_input
 sub qword [input_allocated],IN_CAP
.out:
 mov rdi,[r12+16]
 test rdi,rdi
 jz .clear
 call release_output
 sub qword [output_allocated],OUT_CAP
.clear:
 mov rdi,r12
 mov ecx,8
 xor eax,eax
 rep stosq
 dec qword [active_count]
 shr ebx,16
 shl ebx,9
 lea rax,[ring_contexts]
 dec dword [rax+rbx+116]
.done: LEAVE
; Republish a consumed provided buffer only after its bytes were copied/parsed.
uring_return_buffer:
 ; edi ring index, esi buffer id
 mov eax,edi
 shl rax,9
 lea r8,[ring_contexts]
 add r8,rax
 mov edx,[r8+112]
 mov ecx,edx
 and ecx,NBUFS-1
 shl rcx,4
 add rcx,[r8+96]
 mov eax,esi
 imul rax,BSIZE
 add rax,[r8+104]
 mov [rcx],rax
 mov dword [rcx+8],BSIZE
 mov [rcx+12],si
 inc edx
 mov [r8+112],edx
 mov rax,[r8+96]
 mov [rax+14],dx
 ret
; ENOBUFS retry entries retain no pointers or kernel ownership. One entry per
; paused connection, tagged with its generation; stale entries are harmless.
; Only rearm after consuming the frozen CQ, outside all SQLite transactions.
uring_enobuf_drain:
 ENTER
 xor ebp,ebp
 mov ebx,edi
 mov r12d,edi
 shl r12,9
 lea rax,[ring_contexts]
 add r12,rax
 mov rax,[r12+64]
 mov edx,[rax]
 mov rax,[r12+72]
 cmp edx,[rax]
 jne .done
 xor ebp,ebp
.loop:
 cmp ebp,128
 jae .done
 mov eax,[r12+128]
 cmp eax,[r12+132]
 je .done
 mov ecx,eax
 and ecx,65535
 mov edx,ebx
 shl edx,16
 add edx,ecx
 lea rcx,[enobuf_queue]
 mov r14,[rcx+rdx*8]
 inc eax
 mov [r12+128],eax
 inc ebp
 mov r13d,r14d
 lea rax,[generations]
 shr r14,32
 cmp r14d,[rax+r13*4]
 jne .loop
 mov r15d,r13d
 shl r15,6
 lea rax,[connections]
 add r15,rax
 test dword [r15+60],256
 jz .loop
 and dword [r15+60],~256
 mov edi,r13d
 call uring_recv
 jmp .loop
.done:
 mov eax,ebp
 LEAVE
uring_enobuf_pause:
 ENTER
 mov ebx,edi
 mov r12d,edi
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],256
 jnz .done
 mov r13d,edi
 shr r13d,16
 mov r14d,r13d
 shl r14,9
 lea rax,[ring_contexts]
 add r14,rax
 mov eax,[r14+132]
 mov ecx,eax
 sub ecx,[r14+128]
 cmp ecx,65536
 jae .full
 mov ecx,eax
 and ecx,65535
 shl r13d,16
 add r13d,ecx
 lea rcx,[generations]
 mov edx,[rcx+rbx*4]
 shl rdx,32
 or rdx,rbx
 lea rcx,[enobuf_queue]
 mov [rcx+r13*8],rdx
 inc eax
 mov [r14+132],eax
 or dword [r12+60],256
 jmp .done
.full:
 ; Gracefully finish a reserved response if bounded retry storage is full.
 cmp qword [r12+16],0
 je .close
 or dword [r12+60],2
 jmp .done
.close:
 mov edi,ebx
 call uring_close
.done: LEAVE
; Process up to 64 resource-releasing completions from an already frozen CQ.
; Slots remain owned by userspace until advancing CQ head. Mark handled entries
; so the FIFO pass can publish them exactly once, without retaining CQ pointers.
; edi ring, esi head, edx frozen tail -> eax count. At least half the usual
; completion budget remains available for receive work, preventing starvation.
uring_prioritize:
 ENTER
 mov ebx,edi
 mov r13d,esi
 mov r14d,edx
 mov r12d,edi
 shl r12,9
 lea rax,[ring_contexts]
 add r12,rax
 xor ebp,ebp
.scan:
 cmp r13d,r14d
 je .done
 cmp ebp,64
 jae .done
 mov rax,[r12+80]
 mov eax,[rax]
 and eax,r13d
 shl rax,4
 add rax,[r12+88]
 mov r15,rax
 mov rdx,[rax]
 cmp rdx,-1
 je .next
 shr rdx,20
 and edx,15
 cmp edx,3 ; send completion frees stable response storage
 je .process
 cmp edx,4 ; direct close completion releases remaining resources
 je .process
 cmp edx,6 ; orphan close completion releases fixed-file capacity
 jne .next
.process:
 mov rsi,r15
 mov edi,ebx
 call uring_process
 mov qword [r15],-1
 inc ebp
 test ebp,15
 jnz .next
 mov eax,228
 mov edi,1
 lea rsi,[batch_clock_now]
 syscall
 mov rax,[batch_clock_now]
 sub rax,[batch_clock_start]
 imul rax,rax,1000000000
 add rax,[batch_clock_now+8]
 sub rax,[batch_clock_start+8]
 cmp rax,1000000
 jae .done
.next:
 inc r13d
 jmp .scan
.done:
 mov eax,ebp
 LEAVE
uring_loop:
 ENTER
.loop:
 cmp dword [stop_flag],0
 jne .done
 call clock_update
 call expire_timers
%ifdef IO_DIAG
 IO_HIGH input_allocated,11
 IO_HIGH output_allocated,12
 call io_dump
%endif
 mov qword [rsp],0 ; completed this round
 xor ebx,ebx
.ring:
 cmp ebx,[uring_count]
 jae .after
 mov r12d,ebx
 shl r12,9
 lea rax,[ring_contexts]
 add r12,rax
 cmp dword [enobuf_mode],0
 je .active_check
 mov eax,[r12+128]
 cmp eax,[r12+132]
 jne .enter
.active_check:
 ; Inactive capacity rings consume no enter syscall. Never skip queued completions.
 cmp dword [r12+116],0
 jne .enter
 cmp dword [r12+120],0
 jne .enter
 mov rax,[r12+40]
 mov edx,[rax]
 mov rax,[r12+32]
 cmp edx,[rax]
 jne .enter
 mov rax,[r12+72]
 mov edx,[rax]
 mov rax,[r12+64]
 cmp edx,[rax]
 je .nextring_idle
.enter:
 ; Submit all queued SQEs and service cooperative kernel task work without waiting.
 mov rax,[r12+40]
 mov esi,[rax]
 mov rax,[r12+32]
 sub esi,[rax]
 mov eax,[wait_us]
 test eax,eax
 jz .immediate
 imul rax,rax,1000
 mov [wait_time+8],rax
 lea rax,[wait_time]
 mov [wait_arg+16],rax
 mov edx,[completion_target]
 mov r10d,9
 lea r8,[wait_arg]
 mov r9d,24
 jmp .collect
.immediate:
 xor edx,edx
 mov r10d,1
 xor r8d,r8d
 xor r9d,r9d
.collect:
 IO_INC 0
 mov eax,426
 mov edi,[r12]
 syscall
 cmp eax,-4
 je .ring
 cmp eax,-62
 je .collected
 test eax,eax
 js fatal
.collected:
 mov eax,228
 mov edi,1
 lea rsi,[batch_clock_start]
 syscall
 mov r13,[r12+64]
 mov r14d,[r13]
 mov rax,[r12+72]
 mov r15d,[rax] ; frozen completion boundary, no collection during SQL execution
%ifdef IO_DIAG
 mov eax,r15d
 sub eax,r14d
 cmp rax,[io_counters+8*10]
 jbe .depth_counted
 mov [io_counters+8*10],rax
.depth_counted:
%endif
 xor ebp,ebp
 mov dword [group_context],1
 mov dword [batch_failed],0
 mov dword [reply_count],0
 mov dword [deferred_count],0
 cmp dword [completion_order],0
 je .cqe
 mov edi,ebx
 mov esi,r14d
 mov edx,r15d
 call uring_prioritize
 mov ebp,eax
 add [rsp],rax
.cqe:
 cmp r14d,r15d
 je .nextring
 cmp ebp,128
 jae .nextring
 mov rax,[r12+80]
 mov eax,[rax]
 and eax,r14d
 shl rax,4
 add rax,[r12+88]
 cmp qword [rax],-1
 je .handled
 mov rsi,rax
 mov edi,ebx
 call uring_process
 inc ebp
 inc qword [rsp]
 inc r14d
 mov [r13],r14d
 test ebp,15
 jnz .cqe
 mov eax,228
 mov edi,1
 lea rsi,[batch_clock_now]
 syscall
 mov rax,[batch_clock_now]
 sub rax,[batch_clock_start]
 imul rax,rax,1000000000
 add rax,[batch_clock_now+8]
 sub rax,[batch_clock_start+8]
 cmp rax,1000000
 jae .nextring
 jmp .cqe
.handled:
 inc r14d
 mov [r13],r14d
 jmp .cqe
.nextring:
 call flush_batch
 cmp dword [enobuf_mode],0
 je .nextring_idle
 mov edi,ebx
 call uring_enobuf_drain
 add [rsp],rax
.nextring_idle:
 inc ebx
 jmp .ring
.after:
 cmp qword [rsp],0
 jne .loop
 mov eax,232
 mov edi,[epoll_fd]
 lea rsi,[events]
 mov edx,256
 mov r10d,1000
 syscall
 cmp eax,-4
 je .loop
 test eax,eax
 js .done
 jmp .loop
.done: LEAVE
; Process CQE using only stable local values; kernel may reuse CQ storage after return.
uring_process:
 ENTER
 mov ebx,edi ; ring
 mov r15,[rsi] ; tag
 mov r13d,[rsi+8] ; signed result
 mov r14d,[rsi+12] ; flags
 mov rax,r15
 shr rax,20
 and eax,15
 cmp eax,1
 je .accept
 cmp eax,6
 je .orphan_closed
 mov ebp,r15d
 and ebp,0xfffff
 cmp ebp,MAX_FD
 jae fatal
 mov r12d,ebp
 shl r12,6
 lea rax,[connections]
 add r12,rax
 mov rax,r15
 shr rax,32
 lea rdx,[generations]
 cmp eax,[rdx+rbp*4]
 jne .stale
 mov rax,r15
 shr rax,20
 and eax,15
 cmp eax,2
 je .recv
 cmp eax,3
 je .send
 cmp eax,4
 je .closed
 jmp .done ; cancel confirmation
.accept:
 mov eax,ebx
 shl rax,9
 lea rdx,[ring_contexts]
 dec dword [rdx+rax+120]
 test r13d,r13d
 js .done ; full fixed table is rearmed when sockets close
 cmp r13d,[ring_slots]
 jae fatal
 inc dword [rdx+rax+116]
 mov ebp,ebx
 shl ebp,16
 or ebp,r13d
 mov r12d,ebp
 shl r12,6
 lea rax,[connections]
 add r12,rax
 cmp dword [r12+60],0
 jne .orphan
 cmp qword [input_allocated],28*1024*1024
 jae .orphan
 cmp qword [output_allocated],64*1024*1024
 jae .orphan
 lea rax,[generations]
 inc dword [rax+rbp*4]
 mov dword [r12+60],1
 inc qword [active_count]
 mov eax,ebx
 shl rax,9
 mov ecx,[ring_slots]
 ; Scale the activation margin for small inherited descriptor limits.
 ; A fixed subtraction underflows below 256 slots, preventing the next
 ; registered-file ring from accepting even when it has ample capacity.
 mov edx,ecx
 shr edx,2
 cmp edx,256
 jbe .margin
 mov edx,256
.margin:
 sub ecx,edx
 lea rdx,[ring_contexts]
 cmp [rdx+rax+116],ecx
 jb .timer
 lea edi,[rbx+1]
 cmp edi,[uring_count]
 jae .timer
 call uring_arm_accept
.timer:
 mov edi,ebp
 mov esi,120
 call timer_add
 mov edi,ebp
 call uring_recv
 mov edi,ebx
 call uring_arm_accept
 jmp .done
.orphan:
 IO_INC 7
 mov edi,ebx
 call uring_sqe
 mov byte [rax],19
 lea ecx,[r13d+1]
 mov [rax+44],ecx
 mov edx,ebp
 or edx,0x600000
 mov [rax+32],rdx
 jmp .done
.orphan_closed:
 mov eax,ebx
 shl rax,9
 lea rdx,[ring_contexts]
 dec dword [rdx+rax+116]
 mov edi,ebx
 call uring_arm_accept
 jmp .done
.recv:
 test r14d,2
 jnz .recv_active
 and dword [r12+60],~32
.recv_active:
 IO_INC 1
 test r14d,1 ; selected buffer
 jz .recv_no_buffer
 test r13d,r13d
 jle .return_close
 test dword [r12+60],16
 jnz .return_retire
 cmp qword [r12],0
 jne .copy
 cmp qword [input_allocated],28*1024*1024
 jae .input_pressure
 call allocate_input
 test rax,rax
 js .return_close
 mov [r12],rax
 add qword [input_allocated],IN_CAP
.copy:
 mov rax,[r12+8]
 add rax,r13
 cmp rax,IN_CAP
 ja .return_close
 mov eax,ebx
 shl rax,9
 lea rdx,[ring_contexts]
 mov rsi,[rdx+rax+104]
 mov eax,r14d
 shr eax,16
 imul rax,BSIZE
 add rsi,rax
 mov rdi,[r12]
 add rdi,[r12+8]
 mov ecx,r13d
 rep movsb
 add [r12+8],r13
 mov edi,ebx
 mov esi,r14d
 shr esi,16
 call uring_return_buffer
 mov edi,ebp
 call timer_touch
 cmp qword [r12+24],0
 jne .done
 jmp .parse
.input_pressure:
 IO_INC 3
 jmp .return_close
.return_close:
 mov edi,ebx
 mov esi,r14d
 shr esi,16
 call uring_return_buffer
 jmp .close
.return_retire:
 mov edi,ebx
 mov esi,r14d
 shr esi,16
 call uring_return_buffer
 jmp .retire
.recv_no_buffer:
 test dword [r12+60],16
 jnz .retire
 cmp r13d,-105 ; ENOBUFS: all earlier CQEs return their buffers before next submit
 je .enobuf
 IO_INC 5
 jmp .close
.enobuf:
 IO_INC 2
 cmp dword [enobuf_mode],0
 je .rearm
 mov edi,ebp
 call uring_enobuf_pause
 jmp .done
.parse:
 call .reserve
 test rax,rax
 js .close
 mov rdi,r12
 call parse_http
 test eax,eax
 jz .incomplete
 js .parse_error
 call .reserve
 test rax,rax
 js .close
 cmp dword [request_close],0
 je .dispatch
 or dword [r12+60],2
.dispatch:
 cmp dword [transaction_mode],1
 jne .execute
 cmp dword [method],2
 jne .execute
 mov eax,[deferred_count]
 cmp eax,256
 jae fatal
 lea rdx,[deferred_writes]
 mov [rdx+rax*4],ebp
 inc dword [deferred_count]
 jmp .done
.execute:
 mov edi,ebp
 call uring_finish_request
 jmp .done
.incomplete:
 cmp dword [expect_seen],0
 je .rearm
 test dword [r12+60],4
 jnz .rearm
 call .reserve
 test rax,rax
 js .close
 mov rdi,[r12+16]
 lea rsi,[interim]
 mov ecx,interim_len
 rep movsb
 mov qword [r12+24],interim_len
 mov qword [r12+32],0
 or dword [r12+60],12
 jmp .submit_send
.parse_error:
 mov dword [response_status],400
 cmp eax,-2
 jne .errorbody
 mov dword [response_status],413
.errorbody:
 lea rsi,[err_json]
 lea rdi,[response_body]
 call append_z
 lea rax,[response_body]
 sub rdi,rax
 mov [response_len],rdi
 or dword [r12+60],2
 call .reserve
 test rax,rax
 js .close
 mov rdi,r12
 call build_response
.submit_send:
 ; Release consumed input early, but never release output while an SQE references it.
 cmp qword [r12+8],0
 jne .sendit
 mov rdi,[r12]
 call release_input
 mov qword [r12],0
 sub qword [input_allocated],IN_CAP
.sendit:
 mov edi,ebp
 call uring_send
 jmp .done
.send:
 and dword [r12+60],~64
 test dword [r12+60],16
 jnz .retire
 test r13d,r13d
 jle .send_failed
 add [r12+32],r13
 mov edi,ebp
 call timer_touch
 mov rax,[r12+32]
 cmp rax,[r12+24]
 jb .sendit
 mov rdi,[r12+16]
 call release_output
 mov qword [r12+16],0
 mov qword [r12+24],0
 mov qword [r12+32],0
 sub qword [output_allocated],OUT_CAP
 test dword [r12+60],2
 jnz .close
 and dword [r12+60],~8
 cmp qword [r12+8],0
 jne .parse
 mov edi,ebp
 call timer_touch
.rearm:
 mov edi,ebp
 call uring_recv
 jmp .done
.send_failed:
 IO_INC 6
 jmp .close
.close:
 IO_INC 9
 mov edi,ebp
 call uring_close
 jmp .done
.closed:
 test r13d,r13d
 js fatal
 or dword [r12+60],128
 mov edi,ebx
 call uring_arm_accept
.retire:
 mov edi,ebp
 call uring_retire
 jmp .done
.stale:
 ; A receive CQE always returns ownership even when its connection generation is old.
 mov rax,r15
 shr rax,20
 and eax,15
 cmp eax,2
 jne .done
 test r14d,1
 jz .done
 mov edi,ebx
 mov esi,r14d
 shr esi,16
 call uring_return_buffer
.done: LEAVE
.reserve:
 sub rsp,8
 cmp qword [r12+16],0
 jne .reserved
 cmp qword [output_allocated],64*1024*1024
 jae .nomem
 call allocate_output
 test rax,rax
 js .reserve_exit
 mov [r12+16],rax
 add qword [output_allocated],OUT_CAP
.reserve_exit:
 add rsp,8
 ret
.reserved:
 mov rax,[r12+16]
 jmp .reserve_exit
.nomem:
 IO_INC 4
 mov rax,-12
 jmp .reserve_exit
uring_shutdown:
 ENTER
 ; No userspace buffers may be reused until closing every ring drains kernel ownership.
 xor ebx,ebx
.loop:
 cmp ebx,[uring_count]
 jae .done
 mov eax,ebx
 shl rax,9
 lea rdx,[ring_contexts]
 mov edi,[rdx+rax]
 mov eax,3
 syscall
 inc ebx
 jmp .loop
.done:
 mov dword [uring_enabled],0
 LEAVE

uring_finish_request:
 ENTER
 mov ebp,edi
 mov r12d,edi
 shl r12,6
 lea rax,[connections]
 add r12,rax
 call dispatch
 mov rdi,r12
 call build_response
 mov rcx,[r12+8]
 sub rcx,[request_bytes]
 mov [r12+8],rcx
 mov rsi,[r12]
 add rsi,[request_bytes]
 mov rdi,[r12]
 rep movsb
 and dword [r12+60],~4
 cmp qword [r12+8],0
 jne .queue
 mov rdi,[r12]
 call release_input
 mov qword [r12],0
 sub qword [input_allocated],IN_CAP
.queue:
 mov eax,[reply_count]
 cmp eax,256
 jae fatal
 lea rdx,[batch_replies]
 mov [rdx+rax*4],ebp
 inc dword [reply_count]
 LEAVE
