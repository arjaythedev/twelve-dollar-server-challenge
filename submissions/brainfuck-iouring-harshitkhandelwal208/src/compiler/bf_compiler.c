#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

// Self-contained high-performance x86_64 optimizing Brainfuck compiler
// Translates BF-MMIO protocol engine directly to native assembly with:
//  1. Compile-time static pointer offset displacement (zero runtime pointer registers)
//  2. Branchless [->[-]+<] boolean mismatch normalizer
//  3. Branchless [-] zeroing idiom
//  4. Branchless linear multiply-accumulate loop folding
//  5. Direct MMIO strobe dispatching
void compile_bf_to_x86_64(const char *src, size_t len, FILE *out) {
    // Filter non-BF chars
    char *code = malloc(len + 1);
    size_t n = 0;
    for (size_t k = 0; k < len; k++) {
        char c = src[k];
        if (c == '+' || c == '-' || c == '<' || c == '>' || c == '[' || c == ']' || c == '.' || c == ',') {
            code[n++] = c;
        }
    }
    code[n] = 0;

    fprintf(out, ".text\n");
    fprintf(out, ".globl bf_execute\n");
    fprintf(out, ".type bf_execute, @function\n");
    fprintf(out, ".p2align 4,,15\n");
    fprintf(out, "bf_execute:\n");
    fprintf(out, "    pushq %%r12\n");
    fprintf(out, "    movq %%rdi, %%r12\n\n");

    int64_t ptr = 0;
    size_t i = 0;
    int label_id = 0;

    typedef struct { int lid; int64_t p; } LoopItem;
    LoopItem loop_stack[1024];
    int loop_top = 0;

    while (i < n) {
        char c = code[i];
        if (c == '>') {
            ptr++;
            i++;
        } else if (c == '<') {
            ptr--;
            i++;
        } else if (c == '+') {
            size_t cnt = 0;
            while (i < n && code[i] == '+') { cnt++; i++; }
            int64_t disp = ptr * 8;
            fprintf(out, "    addq $%zu, %ld(%%r12)\n", cnt, (long)disp);
            if (ptr == 7) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_sys\n    movq $0, %ld(%%r12)\n", (long)disp);
            } else if (ptr == 8) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_crypto\n    movq $0, %ld(%%r12)\n", (long)disp);
            } else if (ptr == 16) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_db\n    movq $0, %ld(%%r12)\n", (long)disp);
            } else if (ptr == 32) {
                fprintf(out, "    movq %%r12, %%rdi\n    call bf_mmio_net\n    movq $0, %ld(%%r12)\n", (long)disp);
            }
        } else if (c == '-') {
            size_t cnt = 0;
            while (i < n && code[i] == '-') { cnt++; i++; }
            int64_t disp = ptr * 8;
            fprintf(out, "    subq $%zu, %ld(%%r12)\n", cnt, (long)disp);
        } else if (c == '[') {
            // Check idiom: [->[-]+<]
            if (strncmp(&code[i], "[->[-]+<]", 9) == 0) {
                int64_t disp = ptr * 8;
                int64_t next_disp = (ptr + 1) * 8;
                fprintf(out, "    cmpq $0, %ld(%%r12)\n", (long)disp);
                fprintf(out, "    setne %%al\n");
                fprintf(out, "    movzbq %%al, %%rax\n");
                fprintf(out, "    movq %%rax, %ld(%%r12)\n", (long)next_disp);
                fprintf(out, "    movq $0, %ld(%%r12)\n", (long)disp);
                i += 9;
                continue;
            }

            // Check innermost loop
            char *close_bracket = strchr(&code[i + 1], ']');
            if (close_bracket) {
                size_t sub_len = close_bracket - &code[i + 1];
                char *open_inside = memchr(&code[i + 1], '[', sub_len);
                if (!open_inside) {
                    if (sub_len == 1 && code[i + 1] == '-') {
                        // [-] zero idiom
                        int64_t disp = ptr * 8;
                        fprintf(out, "    movq $0, %ld(%%r12)\n", (long)disp);
                        i += 3;
                        continue;
                    }

                    // Check linear copy / multiply loop
                    int64_t p_inner = 0;
                    int64_t deltas[512] = {0};
                    bool valid_linear = true;

                    for (size_t k = 0; k < sub_len; k++) {
                        char ch = code[i + 1 + k];
                        if (ch == '>') p_inner++;
                        else if (ch == '<') p_inner--;
                        else if (ch == '+') {
                            if (p_inner + 256 >= 0 && p_inner + 256 < 512) deltas[p_inner + 256]++;
                            else valid_linear = false;
                        } else if (ch == '-') {
                            if (p_inner + 256 >= 0 && p_inner + 256 < 512) deltas[p_inner + 256]--;
                            else valid_linear = false;
                        } else {
                            valid_linear = false;
                            break;
                        }
                    }

                    if (valid_linear && p_inner == 0 && deltas[256] == -1) {
                        int64_t src_disp = ptr * 8;
                        fprintf(out, "    movq %ld(%%r12), %%rax\n", (long)src_disp);
                        for (int k = 0; k < 512; k++) {
                            int64_t factor = deltas[k];
                            int64_t off = k - 256;
                            if (off == 0 || factor == 0) continue;
                            int64_t dst_disp = (ptr + off) * 8;
                            if (factor == 1) {
                                fprintf(out, "    addq %%rax, %ld(%%r12)\n", (long)dst_disp);
                            } else if (factor == 10) {
                                fprintf(out, "    leaq (%%rax,%%rax,4), %%rcx\n");
                                fprintf(out, "    leaq (%%rcx,%%rcx), %%rcx\n");
                                fprintf(out, "    addq %%rcx, %ld(%%r12)\n", (long)dst_disp);
                            } else {
                                fprintf(out, "    imulq $%ld, %%rax, %%rcx\n", (long)factor);
                                fprintf(out, "    addq %%rcx, %ld(%%r12)\n", (long)dst_disp);
                            }
                        }
                        fprintf(out, "    movq $0, %ld(%%r12)\n", (long)src_disp);
                        i += sub_len + 2;
                        continue;
                    }
                }
            }

            // Outer loop
            int lid = label_id++;
            loop_stack[loop_top++] = (LoopItem){ .lid = lid, .p = ptr };
            int64_t disp = ptr * 8;
            fprintf(out, "    cmpq $0, %ld(%%r12)\n", (long)disp);
            fprintf(out, "    je .L_loop_end_%d\n", lid);
            fprintf(out, ".p2align 4,,15\n");
            fprintf(out, ".L_loop_start_%d:\n", lid);
            i++;
        } else if (c == ']') {
            LoopItem item = loop_stack[--loop_top];
            int64_t disp = ptr * 8;
            fprintf(out, "    cmpq $0, %ld(%%r12)\n", (long)disp);
            fprintf(out, "    jne .L_loop_start_%d\n", item.lid);
            fprintf(out, ".L_loop_end_%d:\n", item.lid);
            i++;
        } else {
            i++;
        }
    }

    fprintf(out, "    popq %%r12\n");
    fprintf(out, "    ret\n");
    free(code);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.bf> <output.s>\n", argv[0]);
        return 1;
    }
    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("fopen input"); return 1; }
    fseek(fin, 0, SEEK_END);
    long sz = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    char *src = malloc(sz + 1);
    fread(src, 1, sz, fin);
    src[sz] = 0;
    fclose(fin);

    FILE *fout = fopen(argv[2], "w");
    if (!fout) { perror("fopen output"); return 1; }
    compile_bf_to_x86_64(src, sz, fout);
    fclose(fout);
    free(src);
    return 0;
}
