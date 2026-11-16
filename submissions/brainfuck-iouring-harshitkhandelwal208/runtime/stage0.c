#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out) return 1;

    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *raw = malloc(sz + 1);
    fread(raw, 1, sz, in);
    fclose(in);

    char *code = malloc(sz + 1);
    long n = 0;
    for (long i = 0; i < sz; i++) {
        char ch = raw[i];
        if (ch == '>' || ch == '<' || ch == '+' || ch == '-' ||
            ch == '.' || ch == ',' || ch == '[' || ch == ']') {
            code[n++] = ch;
        }
    }
    free(raw);

    fprintf(out, ".text\n.globl bf_execute\n.type bf_execute, @function\nbf_execute:\n");
    fprintf(out, "    pushq %%r12\n    pushq %%r13\n    subq $8, %%rsp\n");
    fprintf(out, "    movq %%rdi, %%r12\n    movq %%rdi, %%r13\n");

    int ptr = 0, lid = 0, stack[65536], top = 0;
    long i = 0;
    while (i < n) {
        if (i + 8 < n && memcmp(&code[i], "[->[-]+<]", 9) == 0) {
            fprintf(out, "    cmpq $0, (%%r12)\n    setne %%al\n    movzbq %%al, %%rax\n    movq %%rax, 8(%%r12)\n    movq $0, (%%r12)\n");
            i += 9;
            continue;
        }
        if (i + 2 < n && memcmp(&code[i], "[-]", 3) == 0) {
            fprintf(out, "    movq $0, (%%r12)\n");
            i += 3;
            continue;
        }
        char c = code[i];
        if (c == '>') { ptr++; fprintf(out, "    addq $8, %%r12\n"); }
        else if (c == '<') { ptr--; fprintf(out, "    subq $8, %%r12\n"); }
        else if (c == '+') {
            fprintf(out, "    incq (%%r12)\n");
            if (ptr == 7) fprintf(out, "    movq %%r13, %%rdi\n    call bf_mmio_sys\n");
            else if (ptr == 8) fprintf(out, "    movq %%r13, %%rdi\n    call bf_mmio_crypto\n");
            else if (ptr == 16) fprintf(out, "    movq %%r13, %%rdi\n    call bf_mmio_db\n");
            else if (ptr == 32) fprintf(out, "    movq %%r13, %%rdi\n    call bf_mmio_net\n");
        }
        else if (c == '-') fprintf(out, "    decq (%%r12)\n");
        else if (c == '.') fprintf(out, "    movq (%%r12), %%rdi\n    call putchar\n");
        else if (c == ',') fprintf(out, "    call getchar\n    cmpl $-1, %%eax\n    jne .L_eok_%d\n    xorl %%eax, %%eax\n.L_eok_%d:\n    movq %%rax, (%%r12)\n", lid, lid);
        else if (c == '[') {
            int l = ++lid; stack[top++] = l;
            fprintf(out, "    cmpq $0, (%%r12)\n    je .L_e_%d\n.L_s_%d:\n", l, l);
        } else if (c == ']') {
            int l = stack[--top];
            fprintf(out, "    cmpq $0, (%%r12)\n    jne .L_s_%d\n.L_e_%d:\n", l, l);
        }
        i++;
    }
    free(code);
    fprintf(out, "    addq $8, %%rsp\n    popq %%r13\n    popq %%r12\n    ret\n");
    fprintf(out, ".weak main\n.globl main\n.type main, @function\nmain:\n");
    fprintf(out, "    pushq %%r12\n    subq $8000000, %%rsp\n    leaq 4000000(%%rsp), %%r12\n");
    fprintf(out, "    movq %%r12, %%rdi\n    call bf_execute\n    addq $8000000, %%rsp\n    popq %%r12\n    xorl %%eax, %%eax\n    ret\n");
    fprintf(out, ".weak bf_mmio_sys\nbf_mmio_sys: ret\n");
    fprintf(out, ".weak bf_mmio_crypto\nbf_mmio_crypto: ret\n");
    fprintf(out, ".weak bf_mmio_db\nbf_mmio_db: ret\n");
    fprintf(out, ".weak bf_mmio_net\nbf_mmio_net: ret\n");
    fclose(out);
    return 0;
}
