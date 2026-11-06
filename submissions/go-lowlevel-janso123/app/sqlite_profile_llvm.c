//go:build cgo && sqlite_llvm_pgo

/* This translation unit is NOT instrumented. Only sqlite3.c is instrumented.
 * Defining this symbol suppresses compiler-rt's automatic file initialization
 * and atexit registration. No default.profraw, online merging, or periodic
 * counter writes may run while SQLite owners are active.
 */
int __llvm_profile_runtime = 0;

/* The public instr_prof_interface.h hides calls behind no-op macros when the
 * caller itself is not instrumented, so declare the compiler-rt ABI explicitly.
 */
extern void __llvm_profile_set_filename(const char *name);
extern int __llvm_profile_write_file(void);

int goal_llvm_profile_export(const char *name) {
    __llvm_profile_set_filename(name);
    return __llvm_profile_write_file();
}
