; Experimental known-length APPEND copies; literals and lengths share one source.
%define health_prefix_TEXT '{"status":"ok","db":"ok","uptime_s":'
%strlen health_prefix_LEN health_prefix_TEXT
%define post_prefix_TEXT '{"post":{"id":'
%strlen post_prefix_LEN post_prefix_TEXT
%define body_prefix_TEXT ',"body":'
%strlen body_prefix_LEN body_prefix_TEXT
%define created_prefix_TEXT ',"created_at":'
%strlen created_prefix_LEN created_prefix_TEXT
%define author_prefix_TEXT ',"author":'
%strlen author_prefix_LEN author_prefix_TEXT
%define post_end_TEXT ',"like_count":0}}'
%strlen post_end_LEN post_end_TEXT
%define feed_prefix_TEXT '{"posts":['
%strlen feed_prefix_LEN feed_prefix_TEXT
%define single_prefix_TEXT '{"post":'
%strlen single_prefix_LEN single_prefix_TEXT
%define id_prefix_TEXT '{"id":'
%strlen id_prefix_LEN id_prefix_TEXT
%define likes_prefix_TEXT ',"like_count":'
%strlen likes_prefix_LEN likes_prefix_TEXT
%define content_header_TEXT `Content-Type: application/json\r\nContent-Length: `
%strlen content_header_LEN content_header_TEXT
%macro LITERAL 1
%1: db %1 %+ _TEXT,0
%endmacro
%macro APPEND 1
 cmp dword [literal_copy_inline],0
 je %%scalar
 lea rsi,[%1]
 %assign %%offset 0
 %assign %%remaining %1 %+ _LEN
 %rep %%remaining/8
  mov rax,[rsi+%%offset]
  mov [rdi+%%offset],rax
  %assign %%offset %%offset+8
 %endrep
 %assign %%remaining %%remaining % 8
 %if %%remaining >= 4
  mov eax,[rsi+%%offset]
  mov [rdi+%%offset],eax
  %assign %%offset %%offset+4
  %assign %%remaining %%remaining-4
 %endif
 %if %%remaining >= 2
  mov ax,[rsi+%%offset]
  mov [rdi+%%offset],ax
  %assign %%offset %%offset+2
  %assign %%remaining %%remaining-2
 %endif
 %if %%remaining
  mov al,[rsi+%%offset]
  mov [rdi+%%offset],al
  %assign %%offset %%offset+1
 %endif
 lea rdi,[rdi+%%offset]
 lea rsi,[rsi+%%offset]
 xor eax,eax
 jmp %%done
%%scalar:
 lea rsi,[%1]
 call append_z
%%done:
%endmacro
