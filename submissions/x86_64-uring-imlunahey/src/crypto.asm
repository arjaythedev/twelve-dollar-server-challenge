; SPDX-License-Identifier: MIT
; Portable SHA-256/HMAC-SHA256 and strict unpadded base64url, SysV AMD64.
default rel
section .rodata
align 16
sha_initial: dd 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
sha_k: dd 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
 dd 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
 dd 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
 dd 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
 dd 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
 dd 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070
 dd 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
 dd 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
section .bss
align 64
hmac_inner: resb 8256
hmac_outer: resb 96
section .text
global sha256, hmac_init, hmac_sha256, b64decode
; sha256(data, length, output[32]); no inaccessible tail reads.
sha256:
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 sub rsp,136
 mov r12,rdi
 mov r13,rsi
 mov r15,rsi
 mov r14,rdx
 lea rsi,[sha_initial]
 mov rdi,rsp
 mov ecx,8
 rep movsd
.blocks:
 cmp r13,64
 jb .tail
 mov rdi,rsp
 mov rsi,r12
 call sha_compress
 add r12,64
 sub r13,64
 jmp .blocks
.tail:
 lea rdi,[rsp+32]
 xor eax,eax
 mov ecx,64
 rep stosb
 lea rdi,[rsp+32]
 mov rsi,r12
 mov rcx,r13
 rep movsb
 mov byte [rdi],0x80
 cmp r13,56
 jb .length
 mov rdi,rsp
 lea rsi,[rsp+32]
 call sha_compress
 lea rdi,[rsp+32]
 xor eax,eax
 mov ecx,64
 rep stosb
.length:
 shl r15,3
 bswap r15
 mov [rsp+88],r15
 mov rdi,rsp
 lea rsi,[rsp+32]
 call sha_compress
 xor ecx,ecx
.out:
 mov eax,[rsp+rcx*4]
 bswap eax
 mov [r14+rcx*4],eax
 inc ecx
 cmp ecx,8
 jb .out
 add rsp,136
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
; SHA block compression: hash state rdi, block rsi.
sha_compress:
 push rbp
 push rbx
 push r12
 push r13
 push r14
 push r15
 sub rsp,256
 mov rbp,rdi
 xor ecx,ecx
.words:
 mov eax,[rsi+rcx*4]
 bswap eax
 mov [rsp+rcx*4],eax
 inc ecx
 cmp ecx,16
 jb .words
.expand:
 mov eax,[rsp+rcx*4-60]
 mov edx,eax
 mov ebx,eax
 ror eax,7
 ror edx,18
 shr ebx,3
 xor eax,edx
 xor eax,ebx
 mov edx,[rsp+rcx*4-8]
 mov ebx,edx
 mov r8d,edx
 ror edx,17
 ror ebx,19
 shr r8d,10
 xor edx,ebx
 xor edx,r8d
 add eax,edx
 add eax,[rsp+rcx*4-64]
 add eax,[rsp+rcx*4-28]
 mov [rsp+rcx*4],eax
 inc ecx
 cmp ecx,64
 jb .expand
 mov eax,[rbp]
 mov ebx,[rbp+4]
 mov ecx,[rbp+8]
 mov edx,[rbp+12]
 mov r8d,[rbp+16]
 mov r9d,[rbp+20]
 mov r10d,[rbp+24]
 mov r11d,[rbp+28]
 lea rsi,[sha_k]
 xor edi,edi
.round:
 mov r12d,r8d
 mov r13d,r8d
 mov r14d,r8d
 ror r12d,6
 ror r13d,11
 ror r14d,25
 xor r12d,r13d
 xor r12d,r14d
 mov r13d,r9d
 xor r13d,r10d
 and r13d,r8d
 xor r13d,r10d
 add r12d,r13d
 add r12d,r11d
 add r12d,[rsi+rdi*4]
 add r12d,[rsp+rdi*4]
 mov r13d,eax
 mov r14d,eax
 mov r15d,eax
 ror r13d,2
 ror r14d,13
 ror r15d,22
 xor r13d,r14d
 xor r13d,r15d
 mov r14d,eax
 or r14d,ebx
 and r14d,ecx
 mov r15d,eax
 and r15d,ebx
 or r14d,r15d
 add r13d,r14d
 mov r11d,r10d
 mov r10d,r9d
 mov r9d,r8d
 lea r8d,[edx+r12d]
 mov edx,ecx
 mov ecx,ebx
 mov ebx,eax
 lea eax,[r12d+r13d]
 inc edi
 cmp edi,64
 jb .round
 add [rbp],eax
 add [rbp+4],ebx
 add [rbp+8],ecx
 add [rbp+12],edx
 add [rbp+16],r8d
 add [rbp+20],r9d
 add [rbp+24],r10d
 add [rbp+28],r11d
 add rsp,256
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbx
 pop rbp
 ret
; hmac_init(secret, len), secret-derived pads only; no token cache.
hmac_init:
 push r12
 push r13
 sub rsp,72
 mov r12,rdi
 mov r13,rsi
 mov rdi,rsp
 xor eax,eax
 mov ecx,64
 rep stosb
 cmp r13,64
 jbe .short
 mov rdi,r12
 mov rsi,r13
 mov rdx,rsp
 call sha256
 jmp .pads
.short:
 mov rdi,rsp
 mov rsi,r12
 mov rcx,r13
 rep movsb
.pads:
 xor ecx,ecx
 lea r8,[hmac_inner]
 lea r9,[hmac_outer]
.loop:
 mov al,[rsp+rcx]
 mov dl,al
 xor al,0x36
 xor dl,0x5c
 mov [r8+rcx],al
 mov [r9+rcx],dl
 inc ecx
 cmp ecx,64
 jb .loop
 add rsp,72
 pop r13
 pop r12
 ret
; hmac_sha256(input,len<=8192,out)
hmac_sha256:
 push r12
 push r13
 push r14
 mov r12,rdx
 mov r13,rsi
 mov rsi,rdi
 lea rdi,[hmac_inner+64]
 mov rcx,r13
 rep movsb
 lea rdi,[hmac_inner]
 lea rsi,[r13+64]
 lea rdx,[hmac_outer+64]
 call sha256
 lea rdi,[hmac_outer]
 mov esi,96
 mov rdx,r12
 call sha256
 pop r14
 pop r13
 pop r12
 ret
; strict b64url decoder(input,length,out,capacity) -> length or -1.
b64decode:
 xor r8d,r8d
 xor r9d,r9d
 xor r10d,r10d
 xor r11d,r11d
.loop:
 test rsi,rsi
 jz .end
 movzx eax,byte [rdi]
 inc rdi
 dec rsi
 cmp al,'0'
 jb .symbol
 cmp al,'9'
 jbe .digit
 cmp al,'A'
 jb .symbol
 cmp al,'Z'
 jbe .upper
 cmp al,'a'
 jb .symbol
 cmp al,'z'
 jbe .lower
 cmp al,'0'
 jb .symbol
 cmp al,'9'
 ja .symbol
 .digit:
 sub eax,'0'-52
 jmp .value
.upper:
 sub eax,'A'
 jmp .value
.lower:
 sub eax,'a'-26
 jmp .value
.symbol:
 cmp al,'-'
 je .dash
 cmp al,'_'
 jne .bad
 mov eax,63
 jmp .value
.dash:
 mov eax,62
.value:
 shl r8d,6
 or r8d,eax
 add r9d,6
 inc r11d
 cmp r9d,8
 jb .loop
 sub r9d,8
 mov eax,r8d
 push rcx
 mov ecx,r9d
 shr eax,cl
 pop rcx
 cmp r10,rcx
 jae .bad
 mov [rdx+r10],al
 inc r10
 jmp .loop
.end:
 and r11d,3
 cmp r11d,1
 je .bad
 mov eax,1
 push rcx
 mov ecx,r9d
 shl eax,cl
 pop rcx
 dec eax
 test eax,r8d
 jnz .bad
 mov rax,r10
 ret
.bad:
 mov rax,-1
 ret
section .note.GNU-stack noalloc noexec nowrite progbits
