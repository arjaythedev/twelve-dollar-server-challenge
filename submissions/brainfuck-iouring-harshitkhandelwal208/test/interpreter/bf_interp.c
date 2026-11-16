#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define TAPE_SIZE 1000000
#define STACK_SIZE 65536

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source.bf> [max_steps]\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open file %s\n", argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *code = malloc(sz + 1);
    long code_len = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '>' || ch == '<' || ch == '+' || ch == '-' ||
            ch == '.' || ch == ',' || ch == '[' || ch == ']') {
            code[code_len++] = (char)ch;
        }
    }
    fclose(f);
    code[code_len] = '\0';

    // Precompute jump targets and validate brackets
    long *jump = malloc(code_len * sizeof(long));
    long stack[STACK_SIZE];
    long top = 0;

    for (long i = 0; i < code_len; i++) {
        if (code[i] == '[') {
            if (top >= STACK_SIZE) {
                fprintf(stderr, "Error: loop nesting depth exceeded\n");
                free(code); free(jump);
                return 1;
            }
            stack[top++] = i;
        } else if (code[i] == ']') {
            if (top == 0) {
                fprintf(stderr, "Error: unmatched closing bracket at position %ld\n", i);
                free(code); free(jump);
                return 1;
            }
            long start = stack[--top];
            jump[start] = i;
            jump[i] = start;
        }
    }

    if (top != 0) {
        fprintf(stderr, "Error: unmatched opening bracket at position %ld\n", stack[top - 1]);
        free(code); free(jump);
        return 1;
    }

    uint64_t *tape = calloc(TAPE_SIZE, sizeof(uint64_t));
    if (!tape) {
        fprintf(stderr, "Error: memory allocation failed\n");
        free(code); free(jump);
        return 1;
    }

    long ptr = TAPE_SIZE / 2; // start in the middle so left movement is supported
    long ip = 0;
    long long steps = 0;
    long long max_steps = -1;
    if (argc > 2) max_steps = atoll(argv[2]);

    while (ip < code_len) {
        if (max_steps > 0 && ++steps > max_steps) {
            fprintf(stderr, "Error: max steps (%lld) exceeded\n", max_steps);
            free(tape); free(code); free(jump);
            return 2;
        }

        char op = code[ip];
        switch (op) {
            case '>':
                if (ptr + 1 >= TAPE_SIZE) {
                    fprintf(stderr, "Error: tape pointer overflow\n");
                    free(tape); free(code); free(jump);
                    return 1;
                }
                ptr++;
                break;
            case '<':
                if (ptr - 1 < 0) {
                    fprintf(stderr, "Error: tape pointer underflow\n");
                    free(tape); free(code); free(jump);
                    return 1;
                }
                ptr--;
                break;
            case '+':
                tape[ptr]++;
                break;
            case '-':
                tape[ptr]--;
                break;
            case '.':
                putchar((int)(tape[ptr] & 0xFF));
                fflush(stdout);
                break;
            case ',': {
                int in_c = getchar();
                if (in_c == EOF) {
                    tape[ptr] = 0;
                } else {
                    tape[ptr] = (uint64_t)(unsigned char)in_c;
                }
                break;
            }
            case '[':
                if (tape[ptr] == 0) {
                    ip = jump[ip];
                }
                break;
            case ']':
                if (tape[ptr] != 0) {
                    ip = jump[ip];
                }
                break;
        }
        ip++;
    }

    free(tape);
    free(code);
    free(jump);
    return 0;
}
