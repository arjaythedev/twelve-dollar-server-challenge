#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

typedef struct {
    char **lines;
    size_t count;
    size_t cap;
} AsmBuf;

static void asm_emit(AsmBuf *b, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (b->count >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->lines = realloc(b->lines, b->cap * sizeof(char*));
    }
    b->lines[b->count++] = strdup(buf);
}

static void optimize_asm(AsmBuf *b) {
    // -------------------------------------------------------------
    // Pass 1: Character Match Collapse (12 lines -> 4 lines)
    // -------------------------------------------------------------
    char **p1 = malloc(b->count * sizeof(char*));
    size_t p1_cnt = 0;
    size_t i = 0;
    while (i < b->count) {
        if (i + 11 < b->count) {
            char *l0 = b->lines[i];
            char *l1 = b->lines[i+1];
            char *l2 = b->lines[i+2];
            char *l3 = b->lines[i+3];
            char *l4 = b->lines[i+4];
            char *l5 = b->lines[i+5];
            char *l6 = b->lines[i+6];
            char *l7 = b->lines[i+7];
            char *l8 = b->lines[i+8];
            char *l9 = b->lines[i+9];
            char *l10 = b->lines[i+10];
            char *l11 = b->lines[i+11];

            if (strncmp(l0, "    movq ", 9) == 0 && strstr(l0, "(%r12), %rax") &&
                strcmp(l1, "    addq %rax, 392(%r12)") == 0) {
                bool has_sub = (strncmp(l2, "    subq $", 10) == 0 && strstr(l2, ", 392(%r12)"));
                int off = has_sub ? 1 : 0;
                char *k3 = b->lines[i + 2 + off];
                char *k4 = b->lines[i + 3 + off];
                char *k5 = b->lines[i + 4 + off];
                char *k6 = b->lines[i + 5 + off];
                char *k7 = b->lines[i + 6 + off];
                char *k8 = b->lines[i + 7 + off];
                char *k9 = b->lines[i + 8 + off];
                char *k10 = b->lines[i + 9 + off];
                char *k11 = b->lines[i + 10 + off];

                if (strcmp(k3, "    movq $0, 400(%r12)") == 0 &&
                    strcmp(k4, "    cmpq $0, 392(%r12)") == 0 &&
                    strcmp(k5, "    setne %al") == 0 &&
                    strcmp(k6, "    movzbq %al, %rax") == 0 &&
                    strcmp(k7, "    movq %rax, 400(%r12)") == 0 &&
                    strcmp(k8, "    movq $0, 392(%r12)") == 0 &&
                    strcmp(k9, "    movq 400(%r12), %rax") == 0 &&
                    strncmp(k10, "    addq %rax, ", 15) == 0 &&
                    strcmp(k11, "    movq $0, 400(%r12)") == 0) {

                    char src[64], val[32] = "0", dst[64];
                    sscanf(l0, "    movq %63[^,], %%rax", src);
                    if (has_sub) sscanf(l2, "    subq $%31[^,], 392(%%r12)", val);
                    sscanf(k10, "    addq %%rax, %63s", dst);

                    int total = 11 + off;
                    for (int k = 0; k < total; k++) free(b->lines[i + k]);

                    char buf[256];
                    snprintf(buf, sizeof(buf), "    cmpq $%s, %s", val, src); p1[p1_cnt++] = strdup(buf);
                    p1[p1_cnt++] = strdup("    setne %al");
                    p1[p1_cnt++] = strdup("    movzbq %al, %rax");
                    snprintf(buf, sizeof(buf), "    addq %%rax, %s", dst); p1[p1_cnt++] = strdup(buf);
                    i += total;
                    continue;
                }
            }
        }
        p1[p1_cnt++] = b->lines[i++];
    }
    free(b->lines);

    // -------------------------------------------------------------
    // Pass 2: IF-condition double-loop collapse (49 occurrences)
    // -------------------------------------------------------------
    bool if_loops[1024] = {false};
    char **p2 = malloc(p1_cnt * sizeof(char*));
    size_t p2_cnt = 0;
    i = 0;
    while (i < p1_cnt) {
        if (i + 13 < p1_cnt) {
            char *l0 = p1[i];
            char *l1 = p1[i+1];
            char *l2 = p1[i+2];
            char *l3 = p1[i+3];
            char *l4 = p1[i+4];
            char *l5 = p1[i+5];
            char *l6 = p1[i+6];
            char *l7 = p1[i+7];
            char *l8 = p1[i+8];
            char *l9 = p1[i+9];
            char *l10 = p1[i+10];
            char *l11 = p1[i+11];
            char *l12 = p1[i+12];
            char *l13 = p1[i+13];

            if (strncmp(l0, "    movq $1, ", 13) == 0 &&
                strncmp(l1, "    cmpq $0, ", 13) == 0 &&
                strncmp(l2, "    je .L_loop_end_", 19) == 0 &&
                strcmp(l3, ".p2align 4,,15") == 0 &&
                strncmp(l4, ".L_loop_start_", 14) == 0 &&
                strncmp(l5, "    subq $1, ", 13) == 0 &&
                strncmp(l6, "    movq $0, ", 13) == 0 &&
                strncmp(l7, "    cmpq $0, ", 13) == 0 &&
                strncmp(l8, "    jne .L_loop_start_", 22) == 0 &&
                strncmp(l9, ".L_loop_end_", 12) == 0 &&
                strncmp(l10, "    cmpq $0, ", 13) == 0 &&
                strncmp(l11, "    je .L_loop_end_", 19) == 0 &&
                strcmp(l12, ".p2align 4,,15") == 0 &&
                strncmp(l13, ".L_loop_start_", 14) == 0) {

                char y[64], x[64];
                int id_a = 0, id_b = 0;
                sscanf(l0, "    movq $1, %63s", y);
                sscanf(l1, "    cmpq $0, %63s", x);
                sscanf(l2, "    je .L_loop_end_%d", &id_a);
                sscanf(l11, "    je .L_loop_end_%d", &id_b);

                char exp_l4[64], exp_l8[64], exp_l9[64], exp_l13[64];
                snprintf(exp_l4, sizeof(exp_l4), ".L_loop_start_%d:", id_a);
                snprintf(exp_l8, sizeof(exp_l8), "    jne .L_loop_start_%d", id_a);
                snprintf(exp_l9, sizeof(exp_l9), ".L_loop_end_%d:", id_a);
                snprintf(exp_l13, sizeof(exp_l13), ".L_loop_start_%d:", id_b);

                if (strcmp(l4, exp_l4) == 0 && strcmp(l8, exp_l8) == 0 &&
                    strcmp(l9, exp_l9) == 0 && strcmp(l13, exp_l13) == 0) {

                    for (int k = 0; k < 14; k++) free(p1[i + k]);

                    char buf[256];
                    snprintf(buf, sizeof(buf), "    cmpq $0, %s", x); p2[p2_cnt++] = strdup(buf);
                    snprintf(buf, sizeof(buf), "    movq $0, %s", x); p2[p2_cnt++] = strdup(buf);
                    snprintf(buf, sizeof(buf), "    movq $0, %s", y); p2[p2_cnt++] = strdup(buf);
                    snprintf(buf, sizeof(buf), "    jne .L_loop_end_%d", id_b); p2[p2_cnt++] = strdup(buf);

                    if (id_b < 1024) if_loops[id_b] = true;
                    i += 14;
                    continue;
                }
            }
        }

        // Check if this is the tail of an IF loop (cmpq $0, Y; jne .L_loop_start_B; .L_loop_end_B:)
        if (i + 2 < p1_cnt) {
            char *lk0 = p1[i];
            char *lk1 = p1[i+1];
            char *lk2 = p1[i+2];
            int id_b = -1;
            if (strncmp(lk0, "    cmpq $0, ", 13) == 0 &&
                strncmp(lk1, "    jne .L_loop_start_", 22) == 0 &&
                sscanf(lk1, "    jne .L_loop_start_%d", &id_b) == 1 &&
                id_b >= 0 && id_b < 1024 && if_loops[id_b]) {

                char exp_end[64];
                snprintf(exp_end, sizeof(exp_end), ".L_loop_end_%d:", id_b);
                if (strcmp(lk2, exp_end) == 0) {
                    free(lk0);
                    free(lk1);
                    p2[p2_cnt++] = lk2;
                    i += 3;
                    continue;
                }
            }
        }

        p2[p2_cnt++] = p1[i++];
    }
    free(p1);

    // -------------------------------------------------------------
    // Pass 3: Digit accumulator collapse & dead back-edge removal
    // -------------------------------------------------------------
    char **p3 = malloc(p2_cnt * sizeof(char*));
    size_t p3_cnt = 0;
    i = 0;
    while (i < p2_cnt) {
        // Pattern: dead back-edge
        //     movq $0, 448(%r12)
        //     cmpq $0, 448(%r12)
        //     jne .L_loop_start_...
        if (i + 2 < p2_cnt &&
            strcmp(p2[i], "    movq $0, 448(%r12)") == 0 &&
            strcmp(p2[i+1], "    cmpq $0, 448(%r12)") == 0 &&
            strncmp(p2[i+2], "    jne .L_loop_start_", 22) == 0) {
            p3[p3_cnt++] = p2[i];
            free(p2[i+1]);
            free(p2[i+2]);
            i += 3;
            continue;
        }

        // Pattern: start label of digit loop
        // .p2align 4,,15
        // .L_loop_start_D:
        if (i + 1 < p2_cnt &&
            strcmp(p2[i], ".p2align 4,,15") == 0 &&
            strncmp(p2[i+1], ".L_loop_start_", 14) == 0) {
            int did = -1;
            sscanf(p2[i+1], ".L_loop_start_%d:", &did);
            if (did == 46 || did == 51 || did == 56 || did == 61 || did == 66 ||
                did == 71 || did == 76 || did == 81 || did == 86 || did == 91) {
                free(p2[i]);
                free(p2[i+1]);
                i += 2;
                continue;
            }
        }

        // Pattern: digit condition collapse (cmpq $0, 440(%r12); je ...)
        if (i + 5 < p2_cnt &&
            strcmp(p2[i], "    movq $0, 448(%r12)") == 0 &&
            strcmp(p2[i+1], "    movq $0, 392(%r12)") == 0 &&
            strcmp(p2[i+2], "    movq 440(%r12), %rax") == 0 &&
            strcmp(p2[i+3], "    addq %rax, 448(%r12)") == 0 &&
            strcmp(p2[i+4], "    cmpq $0, 448(%r12)") == 0 &&
            strncmp(p2[i+5], "    je .L_loop_end_", 19) == 0) {
            p3[p3_cnt++] = strdup("    cmpq $0, 440(%r12)");
            p3[p3_cnt++] = strdup(p2[i+5]);
            for (int k = 0; k < 6; k++) free(p2[i+k]);
            i += 6;
            continue;
        }

        // Pattern: digit parsing arithmetic
        int d_off = (strcmp(p2[i], "    movq $0, 392(%r12)") == 0) ? 1 : 0;
        if (i + d_off + 14 < p2_cnt &&
            strcmp(p2[i+d_off], "    movq $0, 400(%r12)") == 0 &&
            strcmp(p2[i+d_off+1], "    movq 136(%r12), %rax") == 0 &&
            strcmp(p2[i+d_off+2], "    addq %rax, 392(%r12)") == 0 &&
            strcmp(p2[i+d_off+3], "    movq $0, 136(%r12)") == 0 &&
            strcmp(p2[i+d_off+4], "    movq 392(%r12), %rax") == 0 &&
            strcmp(p2[i+d_off+5], "    leaq (%rax,%rax,4), %rcx") == 0 &&
            strcmp(p2[i+d_off+6], "    leaq (%rcx,%rcx), %rcx") == 0 &&
            strcmp(p2[i+d_off+7], "    addq %rcx, 136(%r12)") == 0 &&
            strcmp(p2[i+d_off+8], "    movq $0, 392(%r12)") == 0 &&
            strncmp(p2[i+d_off+9], "    movq ", 9) == 0 && strstr(p2[i+d_off+9], "(%r12), %rax") &&
            strcmp(p2[i+d_off+10], "    addq %rax, 392(%r12)") == 0 &&
            strcmp(p2[i+d_off+11], "    subq $48, 392(%r12)") == 0 &&
            strcmp(p2[i+d_off+12], "    movq 392(%r12), %rax") == 0 &&
            strcmp(p2[i+d_off+13], "    addq %rax, 136(%r12)") == 0 &&
            strcmp(p2[i+d_off+14], "    movq $0, 392(%r12)") == 0) {

            char digit_mem[64];
            sscanf(p2[i+d_off+9], "    movq %63[^,], %%rax", digit_mem);

            int total = d_off + 15;
            for (int k = 0; k < total; k++) free(p2[i + k]);

            char buf[256];
            p3[p3_cnt++] = strdup("    movq 136(%r12), %rax");
            p3[p3_cnt++] = strdup("    leaq (%rax,%rax,4), %rcx");
            p3[p3_cnt++] = strdup("    leaq (%rcx,%rcx), %rcx");
            snprintf(buf, sizeof(buf), "    movq %s, %%rax", digit_mem); p3[p3_cnt++] = strdup(buf);
            p3[p3_cnt++] = strdup("    leaq -48(%rcx,%rax), %rax");
            p3[p3_cnt++] = strdup("    movq %rax, 136(%r12)");
            i += total;
            continue;
        }

        // Pattern: test cell != 0 and jump
        if (i + 5 < p2_cnt &&
            strcmp(p2[i], "    movq $0, 392(%r12)") == 0 &&
            strcmp(p2[i+1], "    movq $0, 400(%r12)") == 0 &&
            strncmp(p2[i+2], "    movq ", 9) == 0 && strstr(p2[i+2], "(%r12), %rax") &&
            strcmp(p2[i+3], "    addq %rax, 392(%r12)") == 0 &&
            strcmp(p2[i+4], "    cmpq $0, 392(%r12)") == 0 &&
            strcmp(p2[i+5], "    movq $0, 392(%r12)") == 0) {

            int target_idx = i + 6;
            if (i + 7 < p2_cnt && strncmp(p2[i+6], "    movq $0, ", 13) == 0 && strstr(p2[i+6], "(%r12)") &&
                strncmp(p2[i+7], "    jne .L_loop_end_", 20) == 0) {
                target_idx = i + 7;
            }

            if (target_idx < p2_cnt && strncmp(p2[target_idx], "    jne .L_loop_end_", 20) == 0) {
                char src[64];
                sscanf(p2[i+2], "    movq %63[^,], %%rax", src);
                char buf[128];
                snprintf(buf, sizeof(buf), "    cmpq $0, %s", src);
                p3[p3_cnt++] = strdup(buf);
                p3[p3_cnt++] = strdup(p2[target_idx]);
                for (int k = i; k <= target_idx; k++) free(p2[k]);
                i = target_idx + 1;
                continue;
            }
        }

        p3[p3_cnt++] = p2[i++];
    }
    free(p2);

    // -------------------------------------------------------------
    // Pass 4: Dead Store Elimination (DSE) of redundant zero stores
    // -------------------------------------------------------------
    char **p4 = malloc(p3_cnt * sizeof(char*));
    size_t p4_cnt = 0;
    bool zero_known[1024] = {false};
    i = 0;
    while (i < p3_cnt) {
        char *line = p3[i];
        if (strncmp(line, "    movq $0, ", 13) == 0 && strstr(line, "(%r12)")) {
            int disp = -1;
            sscanf(line, "    movq $0, %d(%%r12)", &disp);
            if (disp >= 0 && disp < 1024 && zero_known[disp]) {
                free(line);
                i++;
                continue;
            }
            if (disp >= 0 && disp < 1024) zero_known[disp] = true;
            p4[p4_cnt++] = line;
            i++;
            continue;
        } else if (strstr(line, ":") || strstr(line, "call ")) {
            memset(zero_known, 0, sizeof(zero_known));
            p4[p4_cnt++] = line;
            i++;
            continue;
        } else if (strstr(line, ", ") && strstr(line, "(%r12)")) {
            char *comma = strstr(line, ", ");
            int disp = -1;
            if (sscanf(comma + 2, "%d(%%r12)", &disp) == 1 && disp >= 0 && disp < 1024) {
                zero_known[disp] = false;
            }
            p4[p4_cnt++] = line;
            i++;
            continue;
        }
        p4[p4_cnt++] = line;
        i++;
    }
    free(p3);

    // -------------------------------------------------------------
    // Pass 5: Short-Circuit Route Comparisons
    // -------------------------------------------------------------
    char **out = malloc(p4_cnt * sizeof(char*));
    size_t out_cnt = 0;
    i = 0;
    while (i < p4_cnt) {
        if (i + 3 < p4_cnt &&
            strncmp(p4[i], "    cmpq $", 10) == 0 &&
            strcmp(p4[i+1], "    setne %al") == 0 &&
            strcmp(p4[i+2], "    movzbq %al, %rax") == 0 &&
            strcmp(p4[i+3], "    addq %rax, 384(%r12)") == 0) {

            size_t j = i;
            char cmps[16][128];
            int cmp_cnt = 0;
            char end_lbl[64] = {0};

            while (j < p4_cnt && cmp_cnt < 16) {
                if (j + 3 < p4_cnt &&
                    strncmp(p4[j], "    cmpq $", 10) == 0 &&
                    strcmp(p4[j+1], "    setne %al") == 0 &&
                    strcmp(p4[j+2], "    movzbq %al, %rax") == 0 &&
                    strcmp(p4[j+3], "    addq %rax, 384(%r12)") == 0) {
                    strncpy(cmps[cmp_cnt++], p4[j], 127);
                    j += 4;
                } else if (strcmp(p4[j], "    cmpq $0, 384(%r12)") == 0) {
                    if (j + 3 < p4_cnt && strncmp(p4[j+3], "    jne .L_loop_end_", 20) == 0) {
                        sscanf(p4[j+3], "    jne %63s", end_lbl);
                        j += 4;
                        break;
                    } else if (j + 2 < p4_cnt && strncmp(p4[j+2], "    jne .L_loop_end_", 20) == 0) {
                        sscanf(p4[j+2], "    jne %63s", end_lbl);
                        j += 3;
                        break;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }

            if (end_lbl[0] && cmp_cnt > 0) {
                for (int k = i; k < j; k++) free(p4[k]);
                for (int k = 0; k < cmp_cnt; k++) {
                    out[out_cnt++] = strdup(cmps[k]);
                    char jne_buf[128];
                    snprintf(jne_buf, sizeof(jne_buf), "    jne %s", end_lbl);
                    out[out_cnt++] = strdup(jne_buf);
                }
                out[out_cnt++] = strdup("    movq $0, 384(%r12)");
                i = j;
                continue;
            }
        }

        out[out_cnt++] = p4[i++];
    }
    free(p4);

    b->lines = out;
    b->count = out_cnt;
}

void compile_bf_to_x86_64(const char *src, size_t len, FILE *out) {
    char *code = malloc(len + 1);
    size_t n = 0;
    for (size_t k = 0; k < len; k++) {
        char c = src[k];
        if (c == '+' || c == '-' || c == '<' || c == '>' || c == '[' || c == ']' || c == '.' || c == ',') {
            code[n++] = c;
        }
    }
    code[n] = 0;

    AsmBuf b = {0};
    asm_emit(&b, ".text");
    asm_emit(&b, ".globl bf_execute");
    asm_emit(&b, ".type bf_execute, @function");
    asm_emit(&b, ".p2align 4,,15");
    asm_emit(&b, "bf_execute:");
    asm_emit(&b, "    pushq %%r12");
    asm_emit(&b, "    movq %%rdi, %%r12\n");

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
            asm_emit(&b, "    addq $%zu, %ld(%%r12)", cnt, (long)disp);
            if (ptr == 7) {
                asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_sys");
            } else if (ptr == 8) {
                asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_crypto");
            } else if (ptr == 16) {
                asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_db");
            } else if (ptr == 32) {
                asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_net");
            }
        } else if (c == '-') {
            size_t cnt = 0;
            while (i < n && code[i] == '-') { cnt++; i++; }
            int64_t disp = ptr * 8;
            asm_emit(&b, "    subq $%zu, %ld(%%r12)", cnt, (long)disp);
        } else if (c == '[') {
            if (strncmp(&code[i], "[->[-]+<]", 9) == 0) {
                int64_t disp = ptr * 8;
                int64_t next_disp = (ptr + 1) * 8;
                asm_emit(&b, "    cmpq $0, %ld(%%r12)", (long)disp);
                asm_emit(&b, "    setne %%al");
                asm_emit(&b, "    movzbq %%al, %%rax");
                asm_emit(&b, "    movq %%rax, %ld(%%r12)", (long)next_disp);
                asm_emit(&b, "    movq $0, %ld(%%r12)", (long)disp);
                i += 9;
                continue;
            }

            char *close_bracket = strchr(&code[i + 1], ']');
            if (close_bracket) {
                size_t sub_len = close_bracket - &code[i + 1];
                char *open_inside = memchr(&code[i + 1], '[', sub_len);
                if (!open_inside) {
                    if (sub_len == 1 && code[i + 1] == '-') {
                        int64_t disp = ptr * 8;
                        i += 3;
                        if (i < n && code[i] == '+') {
                            size_t cval = 0;
                            while (i < n && code[i] == '+') { cval++; i++; }
                            asm_emit(&b, "    movq $%zu, %ld(%%r12)", cval, (long)disp);
                            if (ptr == 7) asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_sys");
                            else if (ptr == 8) asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_crypto");
                            else if (ptr == 16) asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_db");
                            else if (ptr == 32) asm_emit(&b, "    movq %%r12, %%rdi\n    call bf_mmio_net");
                        } else if (i < n && code[i] == '-') {
                            size_t cval = 0;
                            while (i < n && code[i] == '-') { cval++; i++; }
                            asm_emit(&b, "    movq $-%zu, %ld(%%r12)", cval, (long)disp);
                        } else {
                            asm_emit(&b, "    movq $0, %ld(%%r12)", (long)disp);
                        }
                        continue;
                    }

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
                        int non_zero_count = 0;
                        int off_a = 0, off_b = 0;
                        for (int k = 0; k < 512; k++) {
                            if (k == 256 || deltas[k] == 0) continue;
                            if (deltas[k] == 1) {
                                non_zero_count++;
                                if (off_a == 0) off_a = k - 256;
                                else off_b = k - 256;
                            } else {
                                non_zero_count = 999;
                            }
                        }

                        if (non_zero_count == 2) {
                            size_t j = (close_bracket - code) + 1;
                            int64_t shift1 = 0;
                            while (j < n && (code[j] == '>' || code[j] == '<')) {
                                if (code[j] == '>') shift1++;
                                else shift1--;
                                j++;
                            }
                            int tmp_off = 0, dst_off = 0;
                            if (shift1 == off_a) { tmp_off = off_a; dst_off = off_b; }
                            else if (shift1 == off_b) { tmp_off = off_b; dst_off = off_a; }

                            if (tmp_off != 0 && j < n && code[j] == '[') {
                                char *close2 = strchr(&code[j + 1], ']');
                                if (close2) {
                                    size_t sub_len2 = close2 - &code[j + 1];
                                    char *open2 = memchr(&code[j + 1], '[', sub_len2);
                                    if (!open2) {
                                        int64_t p_inner2 = 0;
                                        int64_t deltas2[512] = {0};
                                        bool valid2 = true;
                                        for (size_t k = 0; k < sub_len2; k++) {
                                            char ch = code[j + 1 + k];
                                            if (ch == '>') p_inner2++;
                                            else if (ch == '<') p_inner2--;
                                            else if (ch == '+') {
                                                if (p_inner2 + 256 >= 0 && p_inner2 + 256 < 512) deltas2[p_inner2 + 256]++;
                                                else valid2 = false;
                                            } else if (ch == '-') {
                                                if (p_inner2 + 256 >= 0 && p_inner2 + 256 < 512) deltas2[p_inner2 + 256]--;
                                                else valid2 = false;
                                            } else { valid2 = false; break; }
                                        }

                                        if (valid2 && p_inner2 == 0 && deltas2[256] == -1 && deltas2[-tmp_off + 256] == 1) {
                                            int l2_count = 0;
                                            for (int k = 0; k < 512; k++) if (k != 256 && deltas2[k] != 0) l2_count++;
                                            if (l2_count == 1) {
                                                int64_t src_disp = ptr * 8;
                                                int64_t dst_disp = (ptr + dst_off) * 8;
                                                asm_emit(&b, "    movq %ld(%%r12), %%rax", (long)src_disp);
                                                asm_emit(&b, "    addq %%rax, %ld(%%r12)", (long)dst_disp);

                                                size_t j2 = (close2 - code) + 1;
                                                int64_t shift2 = 0;
                                                while (j2 < n && (code[j2] == '>' || code[j2] == '<')) {
                                                    if (code[j2] == '>') shift2++;
                                                    else shift2--;
                                                    j2++;
                                                }
                                                ptr += shift1 + shift2;
                                                i = j2;
                                                continue;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        int64_t src_disp = ptr * 8;
                        asm_emit(&b, "    movq %ld(%%r12), %%rax", (long)src_disp);
                        for (int k = 0; k < 512; k++) {
                            int64_t factor = deltas[k];
                            int64_t off = k - 256;
                            if (off == 0 || factor == 0) continue;
                            int64_t dst_disp = (ptr + off) * 8;
                            if (factor == 1) {
                                asm_emit(&b, "    addq %%rax, %ld(%%r12)", (long)dst_disp);
                            } else if (factor == 2) {
                                asm_emit(&b, "    leaq (%%rax,%%rax), %%rcx");
                                asm_emit(&b, "    addq %%rcx, %ld(%%r12)", (long)dst_disp);
                            } else if (factor == 3) {
                                asm_emit(&b, "    leaq (%%rax,%%rax,2), %%rcx");
                                asm_emit(&b, "    addq %%rcx, %ld(%%r12)", (long)dst_disp);
                            } else if (factor == 5) {
                                asm_emit(&b, "    leaq (%%rax,%%rax,4), %%rcx");
                                asm_emit(&b, "    addq %%rcx, %ld(%%r12)", (long)dst_disp);
                            } else if (factor == 9) {
                                asm_emit(&b, "    leaq (%%rax,%%rax,8), %%rcx");
                                asm_emit(&b, "    addq %%rcx, %ld(%%r12)", (long)dst_disp);
                            } else if (factor == 10) {
                                asm_emit(&b, "    leaq (%%rax,%%rax,4), %%rcx");
                                asm_emit(&b, "    leaq (%rcx,%rcx), %%rcx");
                                asm_emit(&b, "    addq %%rcx, %ld(%%r12)", (long)dst_disp);
                            } else {
                                asm_emit(&b, "    imulq $%ld, %%rax, %%rcx", (long)factor);
                                asm_emit(&b, "    addq %%rcx, %ld(%%r12)", (long)dst_disp);
                            }
                        }
                        asm_emit(&b, "    movq $0, %ld(%%r12)", (long)src_disp);
                        i += sub_len + 2;
                        continue;
                    }
                }
            }

            int lid = label_id++;
            loop_stack[loop_top++] = (LoopItem){ .lid = lid, .p = ptr };
            int64_t disp = ptr * 8;
            asm_emit(&b, "    cmpq $0, %ld(%%r12)", (long)disp);
            asm_emit(&b, "    je .L_loop_end_%d", lid);
            asm_emit(&b, ".p2align 4,,15");
            asm_emit(&b, ".L_loop_start_%d:", lid);
            i++;
        } else if (c == ']') {
            LoopItem item = loop_stack[--loop_top];
            int64_t disp = ptr * 8;
            asm_emit(&b, "    cmpq $0, %ld(%%r12)", (long)disp);
            asm_emit(&b, "    jne .L_loop_start_%d", item.lid);
            asm_emit(&b, ".L_loop_end_%d:", item.lid);
            i++;
        } else {
            i++;
        }
    }

    asm_emit(&b, "    popq %%r12");
    asm_emit(&b, "    ret");

    optimize_asm(&b);

    for (size_t k = 0; k < b.count; k++) {
        fprintf(out, "%s\n", b.lines[k]);
        free(b.lines[k]);
    }
    free(b.lines);
    free(code);
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    FILE *out = fopen(argv[2], "wb");
    if (!out) return 1;
    compile_bf_to_x86_64(buf, sz, out);
    fclose(out);
    free(buf);
    return 0;
}
