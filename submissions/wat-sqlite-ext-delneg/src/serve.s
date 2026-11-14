# serve.s: the only native code of this entry, a SQLite extension. `.load ./build/serve` registers serve();
# `SELECT serve('build/app.so')` starts WasmEdge, gives the module three imports and runs its "run" export,
# which never returns: the server is the WAT in app.so. The imports (module "host", all (i64 x 7) -> i64):
#   sys(nr, a1..a6)      a Linux system call, straight to the kernel
#   call(fn, a1..a6)     a native function by address (the WAT finds SQLite's in sqlite3_api_routines)
#   membase()            the native address of the module's linear memory (its other arguments are ignored)
# run(api, ctx) gets the shell's sqlite3_api_routines and serve()'s context (the shell's connection).
        .intel_syntax noprefix
        .text
        .globl  sqlite3_extension_init
sqlite3_extension_init:                         # (db, err, api) -> create_function's result
        sub     rsp, 24
        mov     [rip + api], rdx
        lea     rsi, [rip + s_serve]
        mov     edx, -1                         # any number of arguments
        mov     ecx, 1                          # SQLITE_UTF8
        xor     r8d, r8d
        lea     r9, [rip + serve]
        mov     qword ptr [rsp], 0              # xStep, xFinal
        mov     qword ptr [rsp + 8], 0
        mov     rax, [rip + api]
        call    [rax + 360]                     # sqlite3_create_function
        add     rsp, 24
        ret

serve:                                          # (ctx, argc, argv)
        push    rbx
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, 80                         # 2 stack args, then run()'s 2 WasmEdge_Values (32 bytes each)
        mov     rbx, rdi
        mov     r12, rdx
        lea     rdi, [rsp + 16]
        mov     rsi, [rip + api]
        call    WasmEdge_ValueGenI64@PLT
        lea     rdi, [rsp + 48]
        mov     rsi, rbx
        call    WasmEdge_ValueGenI64@PLT
        mov     rdi, [r12]
        mov     rax, [rip + api]
        call    [rax + 872]                     # sqlite3_value_text(argv[0]): the path of app.so
        mov     r13, rax
        call    WasmEdge_ConfigureCreate@PLT
        mov     r14, rax
        mov     rdi, rax
        mov     esi, 2                          # WasmEdge_RunMode_AOT: load the AOT shared object
        call    WasmEdge_ConfigureSetRunMode@PLT
        mov     rdi, r14
        xor     esi, esi
        call    WasmEdge_VMCreate@PLT
        mov     r14, rax
        mov     edi, 4
        lea     rsi, [rip + s_host]
        call    WasmEdge_ModuleInstanceCreate@PLT
        mov     r15, rax
        call    WasmEdge_ValTypeGenI64@PLT
        lea     rdi, [rip + types]
        mov     ecx, 8
        rep stosq
        lea     rdi, [rip + types]
        mov     esi, 7
        lea     rdx, [rip + types]
        mov     ecx, 1
        call    WasmEdge_FunctionTypeCreate@PLT
        mov     r12, rax
        .macro  import name, len, fn
        mov     rdi, r12
        lea     rsi, [rip + \fn]
        xor     edx, edx
        xor     ecx, ecx
        call    WasmEdge_FunctionInstanceCreate@PLT
        mov     rdi, r15
        mov     esi, \len
        lea     rdx, [rip + \name]
        mov     rcx, rax
        call    WasmEdge_ModuleInstanceAddFunction@PLT
        .endm
        import  s_sys, 3, h_sys
        import  s_call, 4, h_call
        import  s_membase, 7, h_membase
        mov     rdi, r14
        mov     rsi, r15
        call    WasmEdge_VMRegisterModuleFromImport@PLT
        mov     rdi, r14
        mov     rsi, r13
        mov     edx, 3
        lea     rcx, [rip + s_run]
        lea     r8, [rsp + 16]
        mov     r9d, 2
        mov     qword ptr [rsp], 0
        mov     qword ptr [rsp + 8], 0
        call    WasmEdge_VMRunWasmFromFile@PLT
        mov     edi, eax                        # here only if loading failed or run() returned
        call    WasmEdge_ResultGetMessage@PLT
        mov     rdi, rbx
        mov     rsi, rax
        mov     edx, -1
        mov     rax, [rip + api]
        call    [rax + 640]                     # sqlite3_result_error(ctx, message, -1)
        add     rsp, 80
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rbx
        ret

# host functions: (data, frame, in, out) -> WasmEdge_Result; each WasmEdge_Value is 32 bytes, the i64 first
h_sys:
        push    rcx
        mov     r11, rdx
        mov     rax, [r11]
        mov     rdi, [r11 + 32]
        mov     rsi, [r11 + 64]
        mov     rdx, [r11 + 96]
        mov     r10, [r11 + 128]
        mov     r8, [r11 + 160]
        mov     r9, [r11 + 192]
        syscall
        pop     rcx
        mov     [rcx], rax
        xor     eax, eax
        ret

h_call:
        push    rbx
        mov     rbx, rcx
        mov     r11, rdx
        mov     r10, [r11]
        mov     rdi, [r11 + 32]
        mov     rsi, [r11 + 64]
        mov     rdx, [r11 + 96]
        mov     rcx, [r11 + 128]
        mov     r8, [r11 + 160]
        mov     r9, [r11 + 192]
        xor     eax, eax                        # no vector registers, for variadic callees
        call    r10
        mov     [rbx], rax
        pop     rbx
        xor     eax, eax
        ret

h_membase:
        push    rbx
        mov     rbx, rcx
        mov     rdi, rsi
        xor     esi, esi
        call    WasmEdge_CallingFrameGetMemoryInstance@PLT
        mov     rdi, rax
        xor     esi, esi
        mov     edx, 1
        call    WasmEdge_MemoryInstanceGetPointer@PLT
        mov     [rbx], rax
        pop     rbx
        xor     eax, eax
        ret

        .section .rodata
s_serve:   .asciz "serve"
s_host:    .ascii "host"
s_sys:     .ascii "sys"
s_call:    .ascii "call"
s_membase: .ascii "membase"
s_run:     .ascii "run"
        .bss
        .p2align 3
api:    .zero 8
types:  .zero 64
        .section .note.GNU-stack,"",@progbits
