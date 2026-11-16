#!/usr/bin/env python3
"""
Generator for pure Brainfuck self-hosting optimizing compiler: src/compiler/bf_compiler.bf
Generates 100% pure standard Brainfuck (8 instructions: > < + - . , [ ])
Target: Generates x86-64 GNU assembly with RLE coalescing, clear loop lowering,
        boolean normalization [->[-]+<] lowering, and loop label management.
"""

import sys

class BFGen:
    def __init__(self):
        self.code = []
        self.cur_ptr = 0
        self.cells = {}
        self.cell_names = []

    def alloc(self, name):
        if name in self.cells:
            return self.cells[name]
        idx = len(self.cell_names)
        self.cells[name] = idx
        self.cell_names.append(name)
        return idx

    def _target(self, name_or_idx):
        if isinstance(name_or_idx, int):
            return name_or_idx
        return self.cells[name_or_idx]

    def move_to(self, name_or_idx):
        target = self._target(name_or_idx)
        diff = target - self.cur_ptr
        if diff > 0: self.code.append(">" * diff)
        elif diff < 0: self.code.append("<" * (-diff))
        self.cur_ptr = target

    def add(self, name, n=1):
        self.move_to(name)
        if n > 0: self.code.append("+" * n)
        elif n < 0: self.code.append("-" * (-n))

    def clear(self, name):
        self.move_to(name)
        self.code.append("[-]")

    def set(self, name, val):
        self.clear(name)
        self.add(name, val)

    def print_char(self, name):
        self.move_to(name)
        self.code.append(".")

    def read_char(self, name):
        self.move_to(name)
        self.code.append(",")

    def move(self, src, dst):
        self.clear(dst)
        self.move_to(src)
        self.code.append("[")
        self.move_to(dst)
        self.code.append("+")
        self.move_to(src)
        self.code.append("-]")

    def copy(self, src, dst, tmp_name="copy_tmp"):
        self.clear(dst)
        self.clear(tmp_name)
        self.move_to(src)
        self.code.append("[")
        self.move_to(dst)
        self.code.append("+")
        self.move_to(tmp_name)
        self.code.append("+")
        self.move_to(src)
        self.code.append("-]")
        self.move(tmp_name, src)

    def while_nonzero(self, cond, fn):
        self.move_to(cond)
        self.code.append("[")
        fn()
        self.move_to(cond)
        self.code.append("]")

    def if_nonzero(self, cond, fn, level=1):
        tmp = f"if_tmp_{level}"
        tmp_copy = f"if_copy_{level}"
        self.copy(cond, tmp, tmp_copy)
        self.move_to(tmp)
        self.code.append("[")
        fn()
        self.clear(tmp)
        self.code.append("]")

    def if_zero(self, cond, fn, level=1):
        flag = f"ifz_flag_{level}"
        tmp = f"ifz_tmp_{level}"
        tmp_copy = f"ifz_copy_{level}"
        self.set(flag, 1)
        self.copy(cond, tmp, tmp_copy)
        self.move_to(tmp)
        self.code.append("[")
        self.set(flag, 0)
        self.clear(tmp)
        self.code.append("]")
        self.move_to(flag)
        self.code.append("[")
        fn()
        self.clear(flag)
        self.code.append("]")

    def check_eq_const(self, var_name, const_val, match_flag):
        self.copy(var_name, "eq_diff", "eq_copy_tmp")
        self.add("eq_diff", -const_val)
        self.clear("eq_mismatch")
        self.move_to("eq_diff")
        self.code.append("[->[-]+<]")
        
        self.set(match_flag, 1)
        self.move_to("eq_mismatch")
        self.code.append("[")
        self.set(match_flag, 0)
        self.clear("eq_mismatch")
        self.code.append("]")

    def print_str(self, s, acc_cell):
        for ch in s.encode('latin1'):
            self.set(acc_cell, ch)
            self.print_char(acc_cell)

    def get_formatted_code(self):
        raw = "".join(self.code)
        lines = []
        for i in range(0, len(raw), 80):
            lines.append(raw[i:i+80])
        return "\n".join(lines)


def build_compiler():
    g = BFGen()

    # Allocate dedicated primitive scratch cells
    g.alloc("copy_tmp")
    g.alloc("eq_diff")
    g.alloc("eq_mismatch")
    g.alloc("eq_copy_tmp")
    for lvl in range(1, 8):
        g.alloc(f"if_tmp_{lvl}")
        g.alloc(f"if_copy_{lvl}")
        g.alloc(f"ifz_flag_{lvl}")
        g.alloc(f"ifz_tmp_{lvl}")
        g.alloc(f"ifz_copy_{lvl}")

    # Application state variables
    in_ch = g.alloc("in_ch")
    is_eof = g.alloc("is_eof")
    next_label_id = g.alloc("next_label_id")
    stack_depth = g.alloc("stack_depth")
    popped_lbl = g.alloc("popped_lbl")

    MAX_DEPTH = 16
    st_cells = [g.alloc(f"st_{i}") for i in range(MAX_DEPTH)]

    q_len = g.alloc("q_len")
    q = [g.alloc(f"q_{i}") for i in range(9)]

    q0_op = g.alloc("q0_op")
    run_count = g.alloc("run_count")
    num_to_print = g.alloc("num_to_print")

    digits = [g.alloc(f"d_{i}") for i in range(8)]

    # Helper state
    is_match = g.alloc("is_match")
    pat_match = g.alloc("pat_match")
    quot = g.alloc("quot")
    rem = g.alloc("rem")
    countdown = g.alloc("countdown")
    started = g.alloc("started")
    has_space = g.alloc("has_space")
    classified_op = g.alloc("classified_op")
    str_acc = g.alloc("str_acc")
    handled = g.alloc("handled")
    keep_counting = g.alloc("keep_counting")
    err_flag = g.alloc("err_flag")

    def classify_char_op():
        g.clear("classified_op")
        targets = [
            (62, 1), # >
            (60, 2), # <
            (43, 3), # +
            (45, 4), # -
            (46, 5), # .
            (44, 6), # ,
            (91, 7), # [
            (93, 8), # ]
        ]
        for val, op_id in targets:
            g.check_eq_const("in_ch", val, "is_match")
            g.if_nonzero("is_match", lambda op=op_id: g.set("classified_op", op), level=3)

    def pop_queue(count):
        if count == 9:
            for i in range(9):
                g.clear(q[i])
            g.clear("q_len")
        elif count == 3:
            for i in range(6):
                g.move(q[i + 3], q[i])
            for i in range(6, 9):
                g.clear(q[i])
            g.add("q_len", -3)
        elif count == 1:
            for i in range(8):
                g.move(q[i + 1], q[i])
            g.clear(q[8])
            g.add("q_len", -1)

    def append_to_queue():
        for i in range(9):
            g.check_eq_const("q_len", i, "is_match")
            g.if_nonzero("is_match", lambda idx=i: g.copy("classified_op", q[idx]), level=3)
        g.add("q_len", 1)

    def fill_queue():
        def check_space():
            g.check_eq_const("q_len", 9, "is_match")
            g.set("has_space", 1)
            g.if_nonzero("is_match", lambda: g.set("has_space", 0), level=2)
            g.if_nonzero("is_eof", lambda: g.set("has_space", 0), level=2)

        check_space()
        def fill_step():
            g.read_char("in_ch")
            g.check_eq_const("in_ch", 0, "is_match")
            g.if_nonzero("is_match", lambda: g.set("is_eof", 1), level=2)

            def process_char():
                classify_char_op()
                g.check_eq_const("classified_op", 0, "is_match")
                g.if_zero("is_match", append_to_queue, level=3)

            g.if_zero("is_eof", process_char, level=2)
            check_space()

        g.while_nonzero("has_space", fill_step)

    def div10():
        g.clear("quot")
        g.clear("rem")
        g.set("countdown", 10)
        def step():
            g.add("num_to_print", -1)
            g.add("rem", 1)
            g.add("countdown", -1)
            g.check_eq_const("countdown", 0, "is_match")
            def on_10():
                g.clear("rem")
                g.add("quot", 1)
                g.set("countdown", 10)
            g.if_nonzero("is_match", on_10, level=4)
        g.while_nonzero("num_to_print", step)

    def print_num():
        for d in digits:
            g.clear(d)
        for d in digits:
            div10()
            g.move("rem", d)
            g.move("quot", "num_to_print")

        g.clear("started")
        for i in range(7, 0, -1):
            d = digits[i]
            def print_d(d_cell=d):
                g.copy(d_cell, "rem")
                g.add("rem", 48)
                g.print_char("rem")

            g.if_nonzero("started", print_d, level=4)

            def check_start(d_cell=d):
                g.check_eq_const(d_cell, 0, "is_match")
                def on_start():
                    g.set("started", 1)
                    print_d(d_cell)
                g.if_zero("is_match", on_start, level=5)

            g.if_zero("started", check_start, level=4)

        g.copy(digits[0], "rem")
        g.add("rem", 48)
        g.print_char("rem")

    def stack_push(val_var):
        for i in range(MAX_DEPTH):
            g.check_eq_const("stack_depth", i, "is_match")
            g.if_nonzero("is_match", lambda idx=i: g.copy(val_var, st_cells[idx]), level=3)
        g.add("stack_depth", 1)

    def stack_pop():
        g.clear("popped_lbl")
        g.add("stack_depth", -1)
        for i in range(MAX_DEPTH):
            g.check_eq_const("stack_depth", i, "is_match")
            g.if_nonzero("is_match", lambda idx=i: g.move(st_cells[idx], "popped_lbl"), level=3)

    # Header
    header = (
        ".text\n"
        ".globl bf_execute\n"
        ".type bf_execute, @function\n"
        "bf_execute:\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    subq $8, %rsp\n"
        "    movq %rdi, %r12\n"
        "    movq %rdi, %r13\n"
    )
    g.print_str(header, "str_acc")

    g.clear("q_len")
    g.clear("next_label_id")
    g.clear("stack_depth")
    g.clear("is_eof")

    fill_queue()

    def dispatch_loop():
        g.clear("handled")

        # Pattern 1: [->[-]+<]
        g.check_eq_const("q_len", 9, "is_match")
        def check_pat1():
            pat1_ops = [7, 4, 1, 7, 4, 8, 3, 2, 8]
            g.set("pat_match", 1)
            for i in range(9):
                g.check_eq_const(q[i], pat1_ops[i], "is_match")
                g.if_zero("is_match", lambda: g.set("pat_match", 0), level=3)

            def emit_pat1():
                pat1_asm = (
                    "    cmpq $0, (%r12)\n"
                    "    je 1f\n"
                    "    movq $1, 8(%r12)\n"
                    "    movq $0, (%r12)\n"
                    "1:\n"
                )
                g.print_str(pat1_asm, "str_acc")
                pop_queue(9)
                fill_queue()
                g.set("handled", 1)

            g.if_nonzero("pat_match", emit_pat1, level=3)

        g.if_nonzero("is_match", check_pat1, level=2)

        # Pattern 2: [-]
        def check_pat2():
            g.set("pat_match", 1)
            for small_len in [0, 1, 2]:
                g.check_eq_const("q_len", small_len, "is_match")
                g.if_nonzero("is_match", lambda: g.set("pat_match", 0), level=3)

            def check_pat2_tokens():
                pat2_ops = [7, 4, 8]
                for i in range(3):
                    g.check_eq_const(q[i], pat2_ops[i], "is_match")
                    g.if_zero("is_match", lambda: g.set("pat_match", 0), level=4)

                def emit_pat2():
                    g.print_str("    movq $0, (%r12)\n", "str_acc")
                    pop_queue(3)
                    fill_queue()
                    g.set("handled", 1)

                g.if_nonzero("pat_match", emit_pat2, level=4)

            g.if_nonzero("pat_match", check_pat2_tokens, level=3)

        g.if_zero("handled", check_pat2, level=2)

        # Normal instructions
        def handle_normal():
            g.copy(q[0], "q0_op")

            for target_op in [1, 2, 3, 4]:
                g.check_eq_const("q0_op", target_op, "is_match")
                def handle_rle(op=target_op):
                    g.clear("run_count")
                    def count_step():
                        g.add("run_count", 1)
                        pop_queue(1)
                        fill_queue()
                        g.set("keep_counting", 0)
                        g.check_eq_const("q_len", 0, "is_match")
                        def check_next():
                            g.check_eq_const(q[0], op, "is_match")
                            g.if_nonzero("is_match", lambda: g.set("keep_counting", 1), level=5)
                        g.if_zero("is_match", check_next, level=4)

                    g.set("keep_counting", 1)
                    g.while_nonzero("keep_counting", count_step)

                    # Emit
                    if op == 1: # >
                        g.check_eq_const("run_count", 1, "is_match")
                        g.if_nonzero("is_match", lambda: g.print_str("    addq $8, %r12\n", "str_acc"), level=4)
                        def emit_multi():
                            g.print_str("    addq $", "str_acc")
                            g.clear("num_to_print")
                            for _ in range(8):
                                g.copy("run_count", "rem")
                                g.move_to("rem")
                                g.code.append("[")
                                g.add("num_to_print", 1)
                                g.add("rem", -1)
                                g.code.append("]")
                            print_num()
                            g.print_str(", %r12\n", "str_acc")
                        g.if_zero("is_match", emit_multi, level=4)

                    elif op == 2: # <
                        g.check_eq_const("run_count", 1, "is_match")
                        g.if_nonzero("is_match", lambda: g.print_str("    subq $8, %r12\n", "str_acc"), level=4)
                        def emit_multi():
                            g.print_str("    subq $", "str_acc")
                            g.clear("num_to_print")
                            for _ in range(8):
                                g.copy("run_count", "rem")
                                g.move_to("rem")
                                g.code.append("[")
                                g.add("num_to_print", 1)
                                g.add("rem", -1)
                                g.code.append("]")
                            print_num()
                            g.print_str(", %r12\n", "str_acc")
                        g.if_zero("is_match", emit_multi, level=4)

                    elif op == 3: # +
                        g.check_eq_const("run_count", 1, "is_match")
                        g.if_nonzero("is_match", lambda: g.print_str("    incq (%r12)\n", "str_acc"), level=4)
                        def emit_multi():
                            g.print_str("    addq $", "str_acc")
                            g.copy("run_count", "num_to_print")
                            print_num()
                            g.print_str(", (%r12)\n", "str_acc")
                        g.if_zero("is_match", emit_multi, level=4)

                    elif op == 4: # -
                        g.check_eq_const("run_count", 1, "is_match")
                        g.if_nonzero("is_match", lambda: g.print_str("    decq (%r12)\n", "str_acc"), level=4)
                        def emit_multi():
                            g.print_str("    subq $", "str_acc")
                            g.copy("run_count", "num_to_print")
                            print_num()
                            g.print_str(", (%r12)\n", "str_acc")
                        g.if_zero("is_match", emit_multi, level=4)

                g.if_nonzero("is_match", handle_rle, level=3)

            # Op 5: '.'
            g.check_eq_const("q0_op", 5, "is_match")
            def emit_dot():
                g.print_str("    movzbl (%r12), %edi\n    call putchar\n", "str_acc")
                pop_queue(1)
                fill_queue()
            g.if_nonzero("is_match", emit_dot, level=3)

            # Op 6: ','
            g.check_eq_const("q0_op", 6, "is_match")
            def emit_comma():
                g.print_str("    call getchar\n    cmpl $-1, %eax\n    jne 1f\n    xorl %eax, %eax\n1:\n    movq %rax, (%r12)\n", "str_acc")
                pop_queue(1)
                fill_queue()
            g.if_nonzero("is_match", emit_comma, level=3)

            # Op 7: '['
            g.check_eq_const("q0_op", 7, "is_match")
            def emit_loop_start():
                g.add("next_label_id", 1)
                stack_push("next_label_id")
                g.print_str("    cmpq $0, (%r12)\n    je .L_e_", "str_acc")
                g.copy("next_label_id", "num_to_print")
                print_num()
                g.print_str("\n.L_s_", "str_acc")
                g.copy("next_label_id", "num_to_print")
                print_num()
                g.print_str(":\n", "str_acc")
                pop_queue(1)
                fill_queue()
            g.if_nonzero("is_match", emit_loop_start, level=3)

            # Op 8: ']'
            g.check_eq_const("q0_op", 8, "is_match")
            def emit_loop_end():
                g.check_eq_const("stack_depth", 0, "is_match")
                def on_close_err():
                    g.print_str(".error \"unmatched closing bracket\"\n", "str_acc")
                    pop_queue(9)
                    g.set("err_flag", 1)

                g.if_nonzero("is_match", on_close_err, level=4)

                def on_close_ok():
                    stack_pop()
                    g.print_str("    cmpq $0, (%r12)\n    jne .L_s_", "str_acc")
                    g.copy("popped_lbl", "num_to_print")
                    print_num()
                    g.print_str("\n.L_e_", "str_acc")
                    g.copy("popped_lbl", "num_to_print")
                    print_num()
                    g.print_str(":\n", "str_acc")
                    pop_queue(1)
                    fill_queue()

                g.if_zero("err_flag", on_close_ok, level=4)

            g.if_nonzero("is_match", emit_loop_end, level=3)

        g.if_zero("handled", handle_normal, level=2)

    g.while_nonzero("q_len", dispatch_loop)

    # After loop: check stack_depth == 0
    g.check_eq_const("stack_depth", 0, "is_match")
    def on_open_err():
        g.print_str(".error \"unmatched opening bracket\"\n", "str_acc")
        g.set("err_flag", 1)
    g.if_zero("is_match", on_open_err, level=1)

    # If no error, emit Footer:
    def emit_footer():
        footer = (
            "    addq $8, %rsp\n"
            "    popq %rbp\n"
            "    popq %rbx\n"
            "    popq %r15\n"
            "    popq %r14\n"
            "    popq %r13\n"
            "    popq %r12\n"
            "    ret\n"
            ".weak main\n"
            ".globl main\n"
            ".type main, @function\n"
            "main:\n"
            "    pushq %rbp\n"
            "    movq %rsp, %rbp\n"
            "    subq $16, %rsp\n"
            "    movl $8000000, %edi\n"
            "    movl $1, %esi\n"
            "    call calloc\n"
            "    testq %rax, %rax\n"
            "    jz .L_calloc_err\n"
            "    movq %rax, %rbx\n"
            "    leaq 4000000(%rbx), %rdi\n"
            "    call bf_execute\n"
            "    movq %rbx, %rdi\n"
            "    call free\n"
            "    xorl %eax, %eax\n"
            "    leave\n"
            "    ret\n"
            ".L_calloc_err:\n"
            "    movl $1, %edi\n"
            "    call exit\n"
        )
        g.print_str(footer, "str_acc")

    g.if_zero("err_flag", emit_footer, level=1)

    return g

if __name__ == "__main__":
    g = build_compiler()
    code = g.get_formatted_code()
    print(f"Generated Brainfuck compiler: {len(g.code)} operations, {len(code)} bytes formatted.")
    out_path = "src/compiler/bf_compiler.bf"
    if len(sys.argv) > 1:
        out_path = sys.argv[1]
    
    with open(out_path, "w") as f:
        f.write("# SELF HOSTED PURE BRAINFUCK COMPILER\n")
        f.write(code)
        f.write("\n")
    print(f"Saved to {out_path}")
