#!/usr/bin/env python3
"""
Comprehensive Differential Test Suite for Self-Hosted Brainfuck Compiler.
Validates:
1. Independent Reference Interpreter (test/interpreter/bf_interp)
2. Stage 0 Minimal C Bootstrap Compiler (build/stage0)
3. Stage 1 Pure Brainfuck Compiler (build/stage1)
4. Stage 2 Verified Recompiled Compiler (build/stage2)
"""

import os
import sys
import subprocess
import tempfile
import random
import difflib

DIR = os.path.dirname(os.path.abspath(__file__))
SUB_ROOT = os.path.dirname(DIR)
INTERP_BIN = os.path.join(DIR, "interpreter", "bf_interp")
STAGE0_BIN = os.path.join(SUB_ROOT, "build", "stage0")
STAGE1_BIN = os.path.join(SUB_ROOT, "build", "stage1")
STAGE2_BIN = os.path.join(SUB_ROOT, "build", "stage2")

# Inputs for specific programs
INPUTS = {
    "echo.bf": b"Testing Brainfuck Echo 12345!\n",
    "rot13.bf": b"Hello, World!\n",
    "multi_input.bf": b"XYZ",
    "eof_test.bf": b"A",
    "calculator.bf": b"54",
}

def ensure_binaries():
    if not os.path.exists(INTERP_BIN):
        subprocess.run(["gcc", "-O3", "-o", INTERP_BIN, os.path.join(DIR, "interpreter", "bf_interp.c")], check=True)
    if not os.path.exists(STAGE0_BIN):
        subprocess.run(["gcc", "-O3", "-o", STAGE0_BIN, os.path.join(SUB_ROOT, "runtime", "stage0.c")], check=True)
    if not os.path.exists(STAGE1_BIN):
        s1_s = os.path.join(SUB_ROOT, "build", "stage1.s")
        s1_o = os.path.join(SUB_ROOT, "build", "stage1.o")
        subprocess.run([STAGE0_BIN, os.path.join(SUB_ROOT, "src", "compiler", "bf_compiler.bf"), s1_s], check=True)
        subprocess.run(["as", "-o", s1_o, s1_s], check=True)
        subprocess.run(["gcc", "-O3", "-o", STAGE1_BIN, s1_o], check=True)
    if not os.path.exists(STAGE2_BIN):
        s2_s = os.path.join(SUB_ROOT, "build", "stage2.s")
        s2_o = os.path.join(SUB_ROOT, "build", "stage2.o")
        with open(s2_s, "wb") as f_out:
            with open(os.path.join(SUB_ROOT, "src", "compiler", "bf_compiler.bf"), "rb") as f_in:
                subprocess.run([STAGE1_BIN], stdin=f_in, stdout=f_out, check=True)
        subprocess.run(["as", "-o", s2_o, s2_s], check=True)
        subprocess.run(["gcc", "-O3", "-o", STAGE2_BIN, s2_o], check=True)

def run_interp(source_path, input_data=b"", max_steps=5000000):
    cmd = [INTERP_BIN, source_path, str(max_steps)]
    proc = subprocess.run(cmd, input=input_data, capture_output=True)
    return proc.returncode, proc.stdout, proc.stderr

def compile_and_run(compiler_kind, source_path, input_data=b""):
    with tempfile.TemporaryDirectory() as tmpdir:
        s_file = os.path.join(tmpdir, "out.s")
        o_file = os.path.join(tmpdir, "out.o")
        bin_file = os.path.join(tmpdir, "prog")

        if compiler_kind == "stage0":
            proc_c = subprocess.run([STAGE0_BIN, source_path, s_file], capture_output=True)
        elif compiler_kind == "stage1":
            with open(source_path, "rb") as f_in, open(s_file, "wb") as f_out:
                proc_c = subprocess.run([STAGE1_BIN], stdin=f_in, stdout=f_out, stderr=subprocess.PIPE)
        elif compiler_kind == "stage2":
            with open(source_path, "rb") as f_in, open(s_file, "wb") as f_out:
                proc_c = subprocess.run([STAGE2_BIN], stdin=f_in, stdout=f_out, stderr=subprocess.PIPE)
        else:
            raise ValueError(f"Unknown compiler: {compiler_kind}")

        with open(s_file, "rb") as f:
            asm = f.read()

        if b".error" in asm:
            return -1, b"", asm

        # Assemble
        proc_as = subprocess.run(["as", "-o", o_file, s_file], capture_output=True)
        if proc_as.returncode != 0:
            return -1, b"", proc_as.stderr

        # Link
        proc_ld = subprocess.run(["gcc", "-O3", "-o", bin_file, o_file], capture_output=True)
        if proc_ld.returncode != 0:
            return -1, b"", proc_ld.stderr

        # Execute
        proc_run = subprocess.run([bin_file], input=input_data, capture_output=True, timeout=5)
        return proc_run.returncode, proc_run.stdout, proc_run.stderr

def run_differential_test(test_name, source_path, input_data=b"", expect_error=False):
    print(f"  Testing {test_name}...", end=" ", flush=True)

    # 1. Interpreter
    rc_interp, out_interp, err_interp = run_interp(source_path, input_data)

    # 2. Stage 0
    rc_s0, out_s0, err_s0 = compile_and_run("stage0", source_path, input_data)

    # 3. Stage 1
    rc_s1, out_s1, err_s1 = compile_and_run("stage1", source_path, input_data)

    # 4. Stage 2
    rc_s2, out_s2, err_s2 = compile_and_run("stage2", source_path, input_data)

    if expect_error:
        # Verify that all reject the program (either compile-time .error or runtime boundary trap)
        rejected_interp = (rc_interp != 0)
        rejected_s0 = (rc_s0 != 0)
        rejected_s1 = (rc_s1 != 0)
        rejected_s2 = (rc_s2 != 0)
        if rejected_interp and rejected_s0 and rejected_s1 and rejected_s2:
            print("OK [Correctly rejected by interpreter and all compiler stages]")
            return True
        else:
            print(f"FAILED (expected rejection): interp={rc_interp}, s0={rc_s0}, s1={rc_s1}, s2={rc_s2}")
            return False

    # Check for execution failure
    if rc_interp != 0:
        print(f"FAILED in interpreter (rc={rc_interp}): {err_interp.decode(errors='ignore')}")
        return False
    if rc_s0 != 0:
        print(f"FAILED in stage0 (rc={rc_s0}): {err_s0.decode(errors='ignore')}")
        return False
    if rc_s1 != 0:
        print(f"FAILED in stage1 (rc={rc_s1}): {err_s1.decode(errors='ignore')}")
        return False
    if rc_s2 != 0:
        print(f"FAILED in stage2 (rc={rc_s2}): {err_s2.decode(errors='ignore')}")
        return False

    # Verify output match
    if out_interp == out_s0 == out_s1 == out_s2:
        print(f"OK [100% Match: {len(out_interp)} bytes, '{out_interp[:30]}']")
        return True
    else:
        print("FAILED: Output mismatch!")
        print(f"  interp: {out_interp[:50]}")
        print(f"  stage0: {out_s0[:50]}")
        print(f"  stage1: {out_s1[:50]}")
        print(f"  stage2: {out_s2[:50]}")
        return False

def generate_random_bf(seed, num_blocks=12):
    rng = random.Random(seed)
    tokens = []
    ptr = 0

    def move_to(target):
        nonlocal ptr
        diff = target - ptr
        if diff > 0:
            tokens.append('>' * diff)
        elif diff < 0:
            tokens.append('<' * (-diff))
        ptr = target

    # Words for inserting arbitrary comments between instructions (must not contain BF tokens: ><+-.,[])
    comment_words = [" loop", " cell", " ignore_me", " temp_val", " /* note */", " # bf_comment\n", " !@$%^&* "]

    for _ in range(num_blocks):
        if rng.random() < 0.3:
            tokens.append(rng.choice(comment_words))

        btype = rng.choice(["linear", "linear", "clear", "loop", "nested_loop", "deep_nest", "boundary", "repeated", "bool_norm", "print"])
        if btype == "linear":
            target = rng.randint(-15, 15)
            move_to(target)
            tokens.append("+" * rng.randint(1, 15))
        elif btype == "repeated":
            target = rng.randint(-10, 10)
            move_to(target)
            tokens.append("+" * rng.randint(20, 50))
            tokens.append("-" * rng.randint(5, 15))
        elif btype == "boundary":
            target = rng.choice([-80, -50, 50, 80])
            move_to(target)
            tokens.append("+" * rng.randint(1, 5))
            move_to(0)
        elif btype == "clear":
            target = rng.randint(-15, 15)
            move_to(target)
            tokens.append("[-]")
        elif btype == "loop":
            counter_cell = rng.randint(-10, 10)
            move_to(counter_cell)
            tokens.append("[-]" + "+" * rng.randint(1, 4) + "[")
            other_cell = rng.randint(-10, 10)
            if other_cell != counter_cell:
                move_to(other_cell)
                tokens.append("+" * rng.randint(1, 3))
            move_to(counter_cell)
            tokens.append("-]")
        elif btype == "nested_loop":
            c1 = rng.randint(-8, 8)
            c2 = c1 + 1
            c3 = c1 + 2
            move_to(c1)
            tokens.append("[-]" + "+" * rng.randint(1, 3) + "[")
            move_to(c2)
            tokens.append("[-]" + "+" * rng.randint(1, 3) + "[")
            move_to(c3)
            tokens.append("+" * rng.randint(1, 3))
            move_to(c2)
            tokens.append("-]")
            move_to(c1)
            tokens.append("-]")
        elif btype == "deep_nest":
            depth = rng.randint(5, 14)
            base = rng.randint(-5, 0)
            move_to(base)
            tokens.append("[-]+" + "[" * depth + "-" + "]" * depth)
        elif btype == "bool_norm":
            target = rng.randint(-10, 10)
            move_to(target)
            tokens.append("[-]" + "+" * rng.randint(0, 3))
            tokens.append("[->[-]+<]")
        elif btype == "print":
            target = rng.randint(-10, 10)
            move_to(target)
            tokens.append(".")

    # Finish with one print so output is verified
    target = rng.randint(-10, 10)
    move_to(target)
    tokens.append(".")

    return "".join(tokens)

def generate_malformed_bf(seed):
    rng = random.Random(seed)
    mtype = rng.choice(["unmatched_open", "unmatched_close", "excessive_depth", "mismatched_mixed"])
    if mtype == "unmatched_open":
        return "+" * rng.randint(1, 5) + "[" + ">+" * rng.randint(1, 5)
    elif mtype == "unmatched_close":
        return "+" * rng.randint(1, 5) + "]" + ">+" * rng.randint(1, 5)
    elif mtype == "excessive_depth":
        return "[" * 17 + "]" * 17
    else:
        return "++[>++[<]-" + "]" * rng.randint(2, 4)

def main():
    ensure_binaries()
    print("================================================================================")
    print("STARTING DIFFERENTIAL TEST SUITE (INTERPRETER vs STAGE0 vs STAGE1 vs STAGE2)")
    print("================================================================================")

    prog_dir = os.path.join(DIR, "programs")
    test_files = sorted([f for f in os.listdir(prog_dir) if f.endswith(".bf")])

    passed = 0
    total = 0

    print("\n-- Static Test Programs --")
    for f in test_files:
        total += 1
        path = os.path.join(prog_dir, f)
        inp = INPUTS.get(f, b"")
        expect_err = f.startswith("err_")
        if run_differential_test(f, path, inp, expect_err):
            passed += 1

    print("\n-- Deterministic Pseudo-Random Brainfuck Programs (50 seeds) --")
    with tempfile.TemporaryDirectory() as tmpdir:
        for seed in range(101, 151): # 50 randomized valid tests
            total += 1
            code = generate_random_bf(seed, num_blocks=12)
            rand_file = os.path.join(tmpdir, f"rand_{seed}.bf")
            with open(rand_file, "w") as rf:
                rf.write(code + "\n")
            if run_differential_test(f"random_valid_seed_{seed}", rand_file, b""):
                passed += 1

    print("\n-- Deterministic Pseudo-Random Malformed Programs (20 seeds) --")
    with tempfile.TemporaryDirectory() as tmpdir:
        for seed in range(201, 221): # 20 randomized malformed tests
            total += 1
            code = generate_malformed_bf(seed)
            rand_file = os.path.join(tmpdir, f"rand_malformed_{seed}.bf")
            with open(rand_file, "w") as rf:
                rf.write(code + "\n")
            if run_differential_test(f"random_malformed_seed_{seed}", rand_file, b"", expect_error=True):
                passed += 1

    print("\n================================================================================")
    print(f"TEST RESULTS: {passed}/{total} PASSED")
    print("================================================================================")

    if passed != total:
        sys.exit(1)

if __name__ == "__main__":
    main()
