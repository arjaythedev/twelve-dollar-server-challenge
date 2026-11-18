#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int queue[16];
static int q_len = 0;
#define MAX_LOOP_DEPTH 16
static int stack[MAX_LOOP_DEPTH];
static int stack_top = 0;
static int next_label_id = 0;
static int in_eof = 0;

static int next_bf_char(FILE *in) {
    int c;
    while ((c = fgetc(in)) != EOF) {
        if (c == '>' || c == '<' || c == '+' || c == '-' ||
            c == '.' || c == ',' || c == '[' || c == ']') {
            return c;
        }
    }
    return EOF;
}

static void fill_queue(FILE *in) {
    while (q_len < 9 && !in_eof) {
        int c = next_bf_char(in);
        if (c == EOF) {
            in_eof = 1;
            break;
        }
        queue[q_len++] = c;
    }
}

static void pop_queue(int count) {
    for (int i = 0; i < q_len - count; i++) {
        queue[i] = queue[i + count];
    }
    q_len -= count;
}

int main(int argc, char **argv) {
    FILE *in = stdin;
    FILE *out = stdout;
    if (argc > 1 && argv[1][0] != '-') in = fopen(argv[1], "rb");
    if (argc > 2 && argv[2][0] != '-') out = fopen(argv[2], "wb");
    if (!in || !out) return 1;

    fprintf(out, ".text\n.globl bf_execute\n.type bf_execute, @function\nbf_execute:\n");
    fprintf(out, "    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n    pushq %%r15\n    pushq %%rbx\n    pushq %%rbp\n    subq $8, %%rsp\n");
    fprintf(out, "    movq %%rdi, %%r12\n");
    fprintf(out, "    testq %%rsi, %%rsi\n");
    fprintf(out, "    jnz 1f\n");
    fprintf(out, "    leaq -4000000(%%rdi), %%rsi\n");
    fprintf(out, "    leaq 4000000(%%rdi), %%rdx\n");
    fprintf(out, "1:\n");
    fprintf(out, "    movq %%rsi, %%r14\n");
    fprintf(out, "    movq %%rdx, %%r15\n");

    fill_queue(in);

    while (q_len > 0) {
        // Pattern 1: [->[-]+<]
        if (q_len >= 9 &&
            queue[0] == '[' && queue[1] == '-' && queue[2] == '>' &&
            queue[3] == '[' && queue[4] == '-' && queue[5] == ']' &&
            queue[6] == '+' && queue[7] == '<' && queue[8] == ']') {
            fprintf(out, "    cmpq $0, (%%r12)\n    je 1f\n    leaq 8(%%r12), %%rax\n    cmpq %%r15, %%rax\n    jae .L_tape_overflow\n    movq $1, 8(%%r12)\n    movq $0, (%%r12)\n1:\n");
            pop_queue(9);
            fill_queue(in);
            continue;
        }

        // Pattern 2: [-]
        if (q_len >= 3 && queue[0] == '[' && queue[1] == '-' && queue[2] == ']') {
            fprintf(out, "    movq $0, (%%r12)\n");
            pop_queue(3);
            fill_queue(in);
            continue;
        }

        int op = queue[0];
        if (op == '>' || op == '<' || op == '+' || op == '-') {
            long count = 0;
            while (q_len > 0 && queue[0] == op) {
                count++;
                pop_queue(1);
                fill_queue(in);
            }
            if (op == '>') {
                if (count == 1) fprintf(out, "    addq $8, %%r12\n");
                else fprintf(out, "    addq $%ld, %%r12\n", count * 8);
                fprintf(out, "    cmpq %%r15, %%r12\n    jae .L_tape_overflow\n");
            } else if (op == '<') {
                if (count == 1) fprintf(out, "    subq $8, %%r12\n");
                else fprintf(out, "    subq $%ld, %%r12\n", count * 8);
                fprintf(out, "    cmpq %%r14, %%r12\n    jb .L_tape_underflow\n");
            } else if (op == '+') {
                if (count == 1) fprintf(out, "    incq (%%r12)\n");
                else fprintf(out, "    addq $%ld, (%%r12)\n", count);
            } else if (op == '-') {
                if (count == 1) fprintf(out, "    decq (%%r12)\n");
                else fprintf(out, "    subq $%ld, (%%r12)\n", count);
            }
            continue;
        }

        if (op == '.') {
            fprintf(out, "    movzbl (%%r12), %%edi\n    call putchar\n");
            pop_queue(1);
            fill_queue(in);
            continue;
        }

        if (op == ',') {
            fprintf(out, "    call getchar\n    cmpl $-1, %%eax\n    jne 1f\n    xorl %%eax, %%eax\n1:\n    movq %%rax, (%%r12)\n");
            pop_queue(1);
            fill_queue(in);
            continue;
        }

        if (op == '[') {
            if (stack_top >= MAX_LOOP_DEPTH) {
                fprintf(out, ".error \"loop nesting depth exceeded\"\n");
                fprintf(stderr, "Error: loop nesting depth exceeded\n");
                return 1;
            }
            next_label_id++;
            stack[stack_top++] = next_label_id;
            fprintf(out, "    cmpq $0, (%%r12)\n    je .L_e_%d\n.L_s_%d:\n", next_label_id, next_label_id);
            pop_queue(1);
            fill_queue(in);
            continue;
        }

        if (op == ']') {
            if (stack_top == 0) {
                fprintf(out, ".error \"unmatched closing bracket\"\n");
                fprintf(stderr, "Error: unmatched closing bracket\n");
                return 1;
            }
            int lbl = stack[--stack_top];
            fprintf(out, "    cmpq $0, (%%r12)\n    jne .L_s_%d\n.L_e_%d:\n", lbl, lbl);
            pop_queue(1);
            fill_queue(in);
            continue;
        }
    }

    if (stack_top != 0) {
        fprintf(out, ".error \"unmatched opening bracket\"\n");
        fprintf(stderr, "Error: unmatched opening bracket\n");
        return 1;
    }

    fprintf(out, "    addq $8, %%rsp\n    popq %%rbp\n    popq %%rbx\n    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%r12\n    ret\n");
    fprintf(out, ".L_tape_underflow:\n");
    fprintf(out, "    movq $1, %%rax\n    movq $2, %%rdi\n    leaq .L_msg_underflow(%%rip), %%rsi\n    movq $30, %%rdx\n    syscall\n");
    fprintf(out, "    movq $60, %%rax\n    movq $1, %%rdi\n    syscall\n");
    fprintf(out, ".L_tape_overflow:\n");
    fprintf(out, "    movq $1, %%rax\n    movq $2, %%rdi\n    leaq .L_msg_overflow(%%rip), %%rsi\n    movq $29, %%rdx\n    syscall\n");
    fprintf(out, "    movq $60, %%rax\n    movq $1, %%rdi\n    syscall\n");
    fprintf(out, ".weak main\n.globl main\n.type main, @function\nmain:\n");
    fprintf(out, "    pushq %%rbp\n    movq %%rsp, %%rbp\n    subq $16, %%rsp\n");
    fprintf(out, "    movl $8000000, %%edi\n    movl $1, %%esi\n    call calloc\n");
    fprintf(out, "    testq %%rax, %%rax\n    jz .L_calloc_err\n");
    fprintf(out, "    movq %%rax, %%rbx\n    leaq 4000000(%%rbx), %%rdi\n");
    fprintf(out, "    movq %%rbx, %%rsi\n    leaq 8000000(%%rbx), %%rdx\n");
    fprintf(out, "    call bf_execute\n");
    fprintf(out, "    movq %%rbx, %%rdi\n    call free\n");
    fprintf(out, "    xorl %%eax, %%eax\n    leave\n    ret\n");
    fprintf(out, ".L_calloc_err:\n    movl $1, %%edi\n    call exit\n");
    fprintf(out, ".section .rodata\n");
    fprintf(out, ".L_msg_underflow:\n    .ascii \"Error: tape pointer underflow\\n\"\n");
    fprintf(out, ".L_msg_overflow:\n    .ascii \"Error: tape pointer overflow\\n\"\n");

    if (in != stdin) fclose(in);
    if (out != stdout) fclose(out);
    return 0;
}
