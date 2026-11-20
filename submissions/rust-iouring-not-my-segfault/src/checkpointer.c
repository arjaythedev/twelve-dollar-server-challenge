/* The checkpointer: a second connection on the shim VFS "twelve-ckpt" over unix-excl. Its
 * PASSIVE passes run on a fiber that yields at every fsync, which the caller performs (io_uring on
 * ring 0), and after every 1 ms of database page writes. No thread: SQLITE_THREADSAFE stays 0 and
 * only the loop thread calls start, restart, resume and close. Compiled only inside
 * src/sqlite_unit.c, after the amalgamation, whose unixFile and storeLastErrno it uses.
 *
 * Why C: it implements SQLite's VFS extension interface over unix-excl and reaches unixFile and
 * storeLastErrno inside the amalgamation. External entry points: twelve_ckpt_open, twelve_ckpt_start,
 * twelve_ckpt_restart, twelve_ckpt_resume, twelve_ckpt_sync_fd, twelve_ckpt_result and
 * twelve_ckpt_close; every other name is static. Exception to building alone: the file compiles only
 * inside src/sqlite_unit.c after the amalgamation, whose unixFile and storeLastErrno it uses.
 * Owns: the one object ckpt_module (the checkpointer whose run is on the fiber, the shim VFS, its io
 * methods and the registration flag), and per checkpointer its connection, fiber and stack mapping.
 * Threading: single owner thread, the loop thread.
 * Between calls: ckpt_module.current is NULL, and a checkpointer is IDLE, DONE, WAIT_SYNC or
 * WAIT_SLICE, never RUNNING. */

#include <limits.h>
#include <ucontext.h>

#define TWELVE_CKPT_DONE 0
#define TWELVE_CKPT_WAIT_SYNC 1
#define TWELVE_CKPT_WAIT_SLICE 2

/* Pass states; the three waiting or finished states equal the codes start and resume return. */
enum ckpt_state {
  CKPT_DONE = TWELVE_CKPT_DONE,
  CKPT_WAIT_SYNC = TWELVE_CKPT_WAIT_SYNC,
  CKPT_WAIT_SLICE = TWELVE_CKPT_WAIT_SLICE,
  CKPT_IDLE = 3,
  CKPT_RUNNING = 4
};

/* The guard page below the fiber stack and the stack itself, in bytes. */
enum { CKPT_GUARD_BYTES = 4096, CKPT_STACK_BYTES = 262144 };

/* The database page writes a pass makes before it yields, in nanoseconds. */
static const int64_t CKPT_SLICE_NS = 1000000;

#if defined(TWELVE_FAULT_INJECT)
struct ckpt_fault {
  char *fail_path;          /* <path>-fault-ckpt-inline-fail */
  char *delay_path;         /* <path>-fault-ckpt-inline-delay */
};
#else
/* No fault state exists outside fault-inject builds. */
struct ckpt_fault {
  unsigned char unused;
};
#endif

struct twelve_ckpt {
  sqlite3 *db;
  ucontext_t loop;
  ucontext_t fiber;
  void *stack;              /* CKPT_GUARD_BYTES of PROT_NONE, then the fiber's CKPT_STACK_BYTES */
  enum ckpt_state state;
  int sync_fd;
  int sync_result;          /* 0 or the negative errno of the caller's fdatasync */
  struct timespec slice_start;  /* CLOCK_MONOTONIC */
  int rc;
  int log;
  int ckpt;
  struct ckpt_fault fault;
};

/* The module's one mutable object, owned by the loop thread. */
static struct ckpt_module {
  /* Non-NULL only while a fiber pass runs. */
  struct twelve_ckpt *current;
  sqlite3_vfs vfs;
  sqlite3_io_methods io;
  int registered;
} ckpt_module;

/* A shim file: this header, then unix-excl's own file object at SHIM_SIZE. */
struct twelve_file {
  sqlite3_file base;
  sqlite3_file *real;
  int main_db;
};
static const size_t SHIM_SIZE = (sizeof(struct twelve_file) + 7U) & ~(size_t)7U;
static_assert(_Alignof(unixFile) <= 8, "SHIM_SIZE rounds the shim header to 8 bytes");
static_assert(_Alignof(sqlite3_file) <= 8, "SHIM_SIZE rounds the shim header to 8 bytes");

int twelve_ckpt_open(const char *path, struct twelve_ckpt **out);
int twelve_ckpt_start(struct twelve_ckpt *c);
int twelve_ckpt_restart(struct twelve_ckpt *c);
int twelve_ckpt_resume(struct twelve_ckpt *c, int sync_result);
int twelve_ckpt_sync_fd(const struct twelve_ckpt *c);
void twelve_ckpt_result(const struct twelve_ckpt *c, int *rc, int *log, int *ckpt);
int twelve_ckpt_close(struct twelve_ckpt *c);

/* A size this module computed, known to fit an int. */
static int int_of_size(size_t value){
  assert(value <= INT_MAX);
  return (int)value;
}

static sqlite3_vfs *excl_vfs(sqlite3_vfs *vfs){
  return (sqlite3_vfs*)vfs->pAppData;
}

/* The shim header of a file this VFS opened: base is struct twelve_file's first member (C17 6.7.2.1), so the
 * sqlite3_file pointer SQLite passes is the header's address. */
static struct twelve_file *shim_of(sqlite3_file *file){
  return (struct twelve_file *)(void *)file;
}

/* unix-excl's file object is a unixFile whose first member is its sqlite3_file (os_unix.c). */
static unixFile *unix_of(sqlite3_file *real){
  return (unixFile *)(void *)real;
}

static sqlite3_file *real_file(sqlite3_file *file){
  return shim_of(file)->real;
}

/* VFS forwarders: each calls unix-excl's method with the unix-excl object. */
static int shim_delete(sqlite3_vfs *vfs, const char *name, int sync_dir){
  return excl_vfs(vfs)->xDelete(excl_vfs(vfs), name, sync_dir);
}
static int shim_access(sqlite3_vfs *vfs, const char *name, int flags, int *res){
  return excl_vfs(vfs)->xAccess(excl_vfs(vfs), name, flags, res);
}
static int shim_full_pathname(sqlite3_vfs *vfs, const char *name, int n, char *out){
  return excl_vfs(vfs)->xFullPathname(excl_vfs(vfs), name, n, out);
}
static void *shim_dl_open(sqlite3_vfs *vfs, const char *name){
  return excl_vfs(vfs)->xDlOpen(excl_vfs(vfs), name);
}
static void shim_dl_error(sqlite3_vfs *vfs, int n, char *msg){
  excl_vfs(vfs)->xDlError(excl_vfs(vfs), n, msg);
}
static void (*shim_dl_sym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void){
  return excl_vfs(vfs)->xDlSym(excl_vfs(vfs), handle, symbol);
}
static void shim_dl_close(sqlite3_vfs *vfs, void *handle){
  excl_vfs(vfs)->xDlClose(excl_vfs(vfs), handle);
}
static int shim_randomness(sqlite3_vfs *vfs, int n, char *out){
  return excl_vfs(vfs)->xRandomness(excl_vfs(vfs), n, out);
}
static int shim_sleep(sqlite3_vfs *vfs, int microseconds){
  return excl_vfs(vfs)->xSleep(excl_vfs(vfs), microseconds);
}
static int shim_current_time(sqlite3_vfs *vfs, double *now){
  return excl_vfs(vfs)->xCurrentTime(excl_vfs(vfs), now);
}
static int shim_get_last_error(sqlite3_vfs *vfs, int n, char *out){
  return excl_vfs(vfs)->xGetLastError(excl_vfs(vfs), n, out);
}
static int shim_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *now){
  return excl_vfs(vfs)->xCurrentTimeInt64(excl_vfs(vfs), now);
}
static int shim_set_system_call(sqlite3_vfs *vfs, const char *name, sqlite3_syscall_ptr call){
  return excl_vfs(vfs)->xSetSystemCall(excl_vfs(vfs), name, call);
}
static sqlite3_syscall_ptr shim_get_system_call(sqlite3_vfs *vfs, const char *name){
  return excl_vfs(vfs)->xGetSystemCall(excl_vfs(vfs), name);
}
static const char *shim_next_system_call(sqlite3_vfs *vfs, const char *name){
  return excl_vfs(vfs)->xNextSystemCall(excl_vfs(vfs), name);
}

/* I/O forwarders: each calls the real file's method on the real file. */
static int shim_close(sqlite3_file *f){
  return real_file(f)->pMethods->xClose(real_file(f));
}
static int shim_read(sqlite3_file *f, void *buf, int amt, sqlite3_int64 offset){
  return real_file(f)->pMethods->xRead(real_file(f), buf, amt, offset);
}
static int shim_truncate(sqlite3_file *f, sqlite3_int64 size){
  return real_file(f)->pMethods->xTruncate(real_file(f), size);
}
static int shim_file_size(sqlite3_file *f, sqlite3_int64 *size){
  return real_file(f)->pMethods->xFileSize(real_file(f), size);
}
static int shim_lock(sqlite3_file *f, int lock){
  return real_file(f)->pMethods->xLock(real_file(f), lock);
}
static int shim_unlock(sqlite3_file *f, int lock){
  return real_file(f)->pMethods->xUnlock(real_file(f), lock);
}
static int shim_check_reserved_lock(sqlite3_file *f, int *res){
  return real_file(f)->pMethods->xCheckReservedLock(real_file(f), res);
}
static int shim_file_control(sqlite3_file *f, int op, void *arg){
  return real_file(f)->pMethods->xFileControl(real_file(f), op, arg);
}
static int shim_sector_size(sqlite3_file *f){
  return real_file(f)->pMethods->xSectorSize(real_file(f));
}
static int shim_device_characteristics(sqlite3_file *f){
  return real_file(f)->pMethods->xDeviceCharacteristics(real_file(f));
}
static int shim_shm_map(sqlite3_file *f, int region, int size, int extend, void volatile **out){
  return real_file(f)->pMethods->xShmMap(real_file(f), region, size, extend, out);
}
static int shim_shm_lock(sqlite3_file *f, int offset, int n, int flags){
  return real_file(f)->pMethods->xShmLock(real_file(f), offset, n, flags);
}
static void shim_shm_barrier(sqlite3_file *f){
  real_file(f)->pMethods->xShmBarrier(real_file(f));
}
static int shim_shm_unmap(sqlite3_file *f, int delete_flag){
  return real_file(f)->pMethods->xShmUnmap(real_file(f), delete_flag);
}
static int shim_fetch(sqlite3_file *f, sqlite3_int64 offset, int amt, void **out){
  return real_file(f)->pMethods->xFetch(real_file(f), offset, amt, out);
}
static int shim_unfetch(sqlite3_file *f, sqlite3_int64 offset, void *page){
  return real_file(f)->pMethods->xUnfetch(real_file(f), offset, page);
}

#if defined(TWELVE_FAULT_INJECT)
/* The fault-inject trigger names beside the database: SQLITE_NOMEM, with neither name kept, when one fails. */
static int fault_open(struct twelve_ckpt *c, const char *path){
  assert(c != NULL);
  assert(path != NULL);
  c->fault.fail_path = sqlite3_mprintf("%s-fault-ckpt-inline-fail", path);
  c->fault.delay_path = sqlite3_mprintf("%s-fault-ckpt-inline-delay", path);
  if( !c->fault.fail_path || !c->fault.delay_path ){
    sqlite3_free(c->fault.fail_path);
    sqlite3_free(c->fault.delay_path);
    c->fault.fail_path = 0;
    c->fault.delay_path = 0;
    return SQLITE_NOMEM;
  }
  return SQLITE_OK;
}

/* Fault-inject builds only: the fail trigger fails this inline sync with the flag still set; the delay
 * trigger holds it, in 1 ms sleeps, until the file is removed. */
static int fault_check(struct twelve_ckpt *c, unixFile *real){
  assert(c != NULL);
  assert(real != NULL);
  if( access(c->fault.fail_path, F_OK)==0 ){
    storeLastErrno(real, EIO);
    return SQLITE_IOERR_FSYNC;
  }
  while( access(c->fault.delay_path, F_OK)==0 ){
    struct timespec now = {0};
    int got = clock_gettime(CLOCK_MONOTONIC, &now);
    assert(got == 0);
    /* Both terms are below 1,000,000,000 ns, so one carry normalizes the sum. */
    long nsec = now.tv_nsec + CKPT_SLICE_NS;
    const struct timespec deadline = {
      .tv_sec = now.tv_sec + (nsec >= 1000000000L),
      .tv_nsec = nsec >= 1000000000L ? nsec - 1000000000L : nsec,
    };
    int slept = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    while( slept==EINTR ){
      slept = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    assert(slept == 0);
  }
  return SQLITE_OK;
}

static void fault_free(struct twelve_ckpt *c){
  assert(c != NULL);
  sqlite3_free(c->fault.fail_path);
  sqlite3_free(c->fault.delay_path);
}
#else
static int fault_open(struct twelve_ckpt *c, const char *path){
  assert(c != NULL);
  assert(path != NULL);
  return SQLITE_OK;
}

static int fault_check(struct twelve_ckpt *c, unixFile *real){
  assert(c != NULL);
  assert(real != NULL);
  return SQLITE_OK;
}

static void fault_free(struct twelve_ckpt *c){
  assert(c != NULL);
}
#endif

/* During a fiber pass the caller performs the fdatasync and resumes with its result. */
static int shim_sync(sqlite3_file *f, int flags){
  unixFile *real = unix_of(real_file(f));
  struct twelve_ckpt *c = ckpt_module.current;
  if( !c ){
    return real_file(f)->pMethods->xSync(real_file(f), flags);
  }
  c->sync_fd = real->h;
  c->state = CKPT_WAIT_SYNC;
  int swapped = swapcontext(&c->fiber, &c->loop);
  assert(swapped == 0);
  if( c->sync_result!=0 ){
    /* UNIXFILE_DIRSYNC stays set, so the next pass's first sync is the WAL's first sync again. */
    /* The negation is defined: twelve_ckpt_resume asserts sync_result != INT_MIN. */
    storeLastErrno(real, -c->sync_result);
    return SQLITE_IOERR_FSYNC;
  }
  if( (real->ctrlFlags & UNIXFILE_DIRSYNC)!=0 ){
    /* The connection's first WAL sync: unixSync's remainder runs inline, a data sync of the frames written
     * since the yield and the one-time directory fsync, and it clears the flag. */
    int fault = fault_check(c, real);
    if( fault!=SQLITE_OK ){
      return fault;
    }
    return real_file(f)->pMethods->xSync(real_file(f), flags);
  }
  return SQLITE_OK;
}

/* After each database page write, a fiber pass yields once 1 ms has passed since its slice began. */
static int shim_write(sqlite3_file *f, const void *buf, int amt, sqlite3_int64 offset){
  int rc = real_file(f)->pMethods->xWrite(real_file(f), buf, amt, offset);
  struct twelve_ckpt *c = ckpt_module.current;
  if( c && shim_of(f)->main_db ){
    struct timespec now = {0};
    sqlite3_int64 elapsed = 0;
    int got = clock_gettime(CLOCK_MONOTONIC, &now);
    assert(got == 0);
    elapsed = (sqlite3_int64)(now.tv_sec - c->slice_start.tv_sec) * 1000000000
            + (sqlite3_int64)(now.tv_nsec - c->slice_start.tv_nsec);
    if( elapsed>=CKPT_SLICE_NS ){
      c->state = CKPT_WAIT_SLICE;
      int swapped = swapcontext(&c->fiber, &c->loop);
      assert(swapped == 0);
    }
  }
  return rc;
}

/* Opens every file through unix-excl's own xOpen and vfs object, so UNIXFILE_EXCL applies, the same locking as store.rs's unix-excl connection. */
static int shim_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *file, int flags, int *out_flags){
  struct twelve_file *shim = shim_of(file);
  /* unix-excl's file object starts SHIM_SIZE bytes in, inside the szOsFile bytes SQLite allocated for this
   * VFS, an offset aligned for it (SHIM_SIZE's static_asserts). */
  sqlite3_file *real = (sqlite3_file *)(void *)((char *)file + SHIM_SIZE);
  shim->base.pMethods = 0;
  int rc = excl_vfs(vfs)->xOpen(excl_vfs(vfs), name, real, flags, out_flags);
  if( real->pMethods ){
    ckpt_module.io.iVersion = real->pMethods->iVersion;
    shim->base.pMethods = &ckpt_module.io;
    shim->real = real;
    shim->main_db = (flags & SQLITE_OPEN_MAIN_DB)!=0;
  }
  return rc;
}

/* The fiber's whole life: one PASSIVE pass, then back to the loop for good. */
static void fiber_entry(void){
  struct twelve_ckpt *c = ckpt_module.current;
  c->rc = sqlite3_wal_checkpoint_v2(c->db, 0, SQLITE_CHECKPOINT_PASSIVE, &c->log, &c->ckpt);
  c->state = CKPT_DONE;
  int swapped = swapcontext(&c->fiber, &c->loop);
  assert(swapped == 0);
}

/* A restart write's whole life: user_version rewritten at its own value, so the COMMIT on a fully copied
 * WAL restarts it and its header sync yields like any other; then back to the loop for good. A failed step
 * skips the rest, and an open transaction is rolled back with the first error kept. */
static void restart_entry(void){
  struct twelve_ckpt *c = ckpt_module.current;
  sqlite3_stmt *version = 0;
  char sql[32]; /* written before read: sqlite3_snprintf fills it */
  int value = 0;
  int rc = sqlite3_exec(c->db, "BEGIN IMMEDIATE", 0, 0, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(c->db, "PRAGMA user_version", -1, &version, 0);
  }
  if( rc==SQLITE_OK ){
    int step = sqlite3_step(version);
    if( step==SQLITE_ROW ){
      value = sqlite3_column_int(version, 0);
    }else{
      rc = step==SQLITE_DONE ? SQLITE_ERROR : step;
    }
    /* sqlite3_finalize repeats the step's code, which rc already holds. */
    (void)sqlite3_finalize(version);
  }
  if( rc==SQLITE_OK ){
    sqlite3_snprintf(int_of_size(sizeof(sql)), sql, "PRAGMA user_version=%d", value);
    rc = sqlite3_exec(c->db, sql, 0, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_exec(c->db, "COMMIT", 0, 0, 0);
  }
  if( sqlite3_get_autocommit(c->db)==0 ){
    /* The first error is the one reported. */
    (void)sqlite3_exec(c->db, "ROLLBACK", 0, 0, 0);
  }
  c->rc = rc;
  c->log = -1;
  c->ckpt = -1;
  c->state = CKPT_DONE;
  int swapped = swapcontext(&c->fiber, &c->loop);
  assert(swapped == 0);
}

/* Runs the fiber until its next yield or its end and returns the state reached. */
static int fiber_run(struct twelve_ckpt *c){
  ckpt_module.current = c;
  int got = clock_gettime(CLOCK_MONOTONIC, &c->slice_start);
  assert(got == 0);
  c->state = CKPT_RUNNING;
  int swapped = swapcontext(&c->loop, &c->fiber);
  assert(swapped == 0);
  ckpt_module.current = 0;
  return c->state;
}

/* Prepares a fresh fiber on the guarded stack for one entry and runs it until its first stop. */
static int fiber_begin(struct twelve_ckpt *c, void (*entry)(void)){
  int got = getcontext(&c->fiber);
  assert(got == 0);
  c->fiber.uc_stack.ss_sp = (char*)c->stack + CKPT_GUARD_BYTES;
  c->fiber.uc_stack.ss_size = CKPT_STACK_BYTES;
  c->fiber.uc_link = 0;
  makecontext(&c->fiber, entry, 0);
  return fiber_run(c);
}

int twelve_ckpt_open(const char *path, struct twelve_ckpt **out){ // NOLINT(readability-function-size): one straight-line startup sequence (register the VFS once, map the stack, allocate, open, pragmas, two read-backs) whose steps share the object and run once
  int saved_errno = errno;
  assert(path != NULL);
  assert(out != NULL);
  struct twelve_ckpt *c = 0;
  sqlite3_stmt *mode = 0;
  sqlite3_stmt *keys = 0;
  void *stack = MAP_FAILED;
  int rc = SQLITE_OK;
  *out = 0;

  /* The shim VFS, registered once per process; a failed registration is retried by the next open. */
  if( !ckpt_module.registered ){
    sqlite3_vfs *excl = sqlite3_vfs_find("unix-excl");
    int size = 0;
    if( !excl ){
      rc = SQLITE_ERROR;
      goto out_restore_errno;
    }
    if( __builtin_add_overflow(int_of_size(SHIM_SIZE), excl->szOsFile, &size) ){
      rc = SQLITE_ERROR;
      goto out_restore_errno;
    }
    ckpt_module.vfs = *excl;
    ckpt_module.vfs.pNext = 0;
    ckpt_module.vfs.zName = "twelve-ckpt";
    ckpt_module.vfs.szOsFile = size;
    ckpt_module.vfs.pAppData = excl;
    ckpt_module.vfs.xOpen = shim_open;
    ckpt_module.vfs.xDelete = excl->xDelete ? shim_delete : 0;
    ckpt_module.vfs.xAccess = excl->xAccess ? shim_access : 0;
    ckpt_module.vfs.xFullPathname = excl->xFullPathname ? shim_full_pathname : 0;
    ckpt_module.vfs.xDlOpen = excl->xDlOpen ? shim_dl_open : 0;
    ckpt_module.vfs.xDlError = excl->xDlError ? shim_dl_error : 0;
    ckpt_module.vfs.xDlSym = excl->xDlSym ? shim_dl_sym : 0;
    ckpt_module.vfs.xDlClose = excl->xDlClose ? shim_dl_close : 0;
    ckpt_module.vfs.xRandomness = excl->xRandomness ? shim_randomness : 0;
    ckpt_module.vfs.xSleep = excl->xSleep ? shim_sleep : 0;
    ckpt_module.vfs.xCurrentTime = excl->xCurrentTime ? shim_current_time : 0;
    ckpt_module.vfs.xGetLastError = excl->xGetLastError ? shim_get_last_error : 0;
    if( excl->iVersion>=2 ){
      ckpt_module.vfs.xCurrentTimeInt64 = excl->xCurrentTimeInt64 ? shim_current_time_int64 : 0;
    }
    if( excl->iVersion>=3 ){
      ckpt_module.vfs.xSetSystemCall = excl->xSetSystemCall ? shim_set_system_call : 0;
      ckpt_module.vfs.xGetSystemCall = excl->xGetSystemCall ? shim_get_system_call : 0;
      ckpt_module.vfs.xNextSystemCall = excl->xNextSystemCall ? shim_next_system_call : 0;
    }
    ckpt_module.io.xClose = shim_close;
    ckpt_module.io.xRead = shim_read;
    ckpt_module.io.xWrite = shim_write;
    ckpt_module.io.xTruncate = shim_truncate;
    ckpt_module.io.xSync = shim_sync;
    ckpt_module.io.xFileSize = shim_file_size;
    ckpt_module.io.xLock = shim_lock;
    ckpt_module.io.xUnlock = shim_unlock;
    ckpt_module.io.xCheckReservedLock = shim_check_reserved_lock;
    ckpt_module.io.xFileControl = shim_file_control;
    ckpt_module.io.xSectorSize = shim_sector_size;
    ckpt_module.io.xDeviceCharacteristics = shim_device_characteristics;
    ckpt_module.io.xShmMap = shim_shm_map;
    ckpt_module.io.xShmLock = shim_shm_lock;
    ckpt_module.io.xShmBarrier = shim_shm_barrier;
    ckpt_module.io.xShmUnmap = shim_shm_unmap;
    ckpt_module.io.xFetch = shim_fetch;
    ckpt_module.io.xUnfetch = shim_unfetch;
    rc = sqlite3_vfs_register(&ckpt_module.vfs, 0);
    if( rc!=SQLITE_OK ){
      goto out_restore_errno;
    }
    ckpt_module.registered = 1;
  }

  stack = mmap(0, CKPT_GUARD_BYTES + CKPT_STACK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if( stack==MAP_FAILED ){
    rc = SQLITE_NOMEM;
    goto out_restore_errno;
  }
  if( mprotect(stack, CKPT_GUARD_BYTES, PROT_NONE)!=0 ){
    rc = SQLITE_NOMEM;
    goto out_unmap;
  }
  c = sqlite3_malloc64(sizeof(*c));
  if( !c ){
    rc = SQLITE_NOMEM;
    goto out_unmap;
  }
  memset(c, 0, sizeof(*c));
  c->stack = stack;
  c->state = CKPT_IDLE;
  rc = fault_open(c, path);
  if( rc!=SQLITE_OK ){
    goto out_free_ckpt;
  }
  rc = sqlite3_open_v2(path, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "twelve-ckpt");
  if( !c->db ){
    /* SQLite could not allocate the handle, so there is none to close. */
    goto out_free_fault;
  }
  if( rc!=SQLITE_OK ){
    goto out_close_db;
  }
  rc = sqlite3_exec(c->db,
    "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0; PRAGMA journal_size_limit=41943040; "
    "PRAGMA mmap_size=0; PRAGMA cache_size=-1024; PRAGMA busy_timeout=0; PRAGMA foreign_keys=ON;", 0, 0, 0);
  if( rc!=SQLITE_OK ){
    goto out_close_db;
  }
  rc = sqlite3_prepare_v2(c->db, "PRAGMA journal_mode=WAL", -1, &mode, 0);
  if( rc!=SQLITE_OK ){
    goto out_close_db;
  }
  {
    int step = sqlite3_step(mode);
    if( step==SQLITE_ROW ){
      const unsigned char *text = sqlite3_column_text(mode, 0);
      rc = text && strcmp((const char*)text, "wal")==0 ? SQLITE_OK : SQLITE_ERROR;
    }else{
      rc = step==SQLITE_DONE ? SQLITE_ERROR : step;
    }
    /* sqlite3_finalize repeats the step's code, which rc already holds. */
    (void)sqlite3_finalize(mode);
  }
  if( rc!=SQLITE_OK ){
    goto out_close_db;
  }
  rc = sqlite3_prepare_v2(c->db, "PRAGMA foreign_keys", -1, &keys, 0);
  if( rc!=SQLITE_OK ){
    goto out_close_db;
  }
  {
    int step = sqlite3_step(keys);
    if( step==SQLITE_ROW ){
      int on = sqlite3_column_type(keys, 0)==SQLITE_INTEGER && sqlite3_column_int(keys, 0)==1;
      rc = on ? SQLITE_OK : SQLITE_ERROR;
    }else{
      rc = step==SQLITE_DONE ? SQLITE_ERROR : step;
    }
    /* sqlite3_finalize repeats the step's code, which rc already holds. */
    (void)sqlite3_finalize(keys);
  }
  if( rc!=SQLITE_OK ){
    goto out_close_db;
  }
  *out = c;
  goto out_restore_errno;

out_close_db:
  /* The open already failed with rc, the code reported. */
  (void)sqlite3_close(c->db);
out_free_fault:
  fault_free(c);
out_free_ckpt:
  sqlite3_free(c);
out_unmap:
  /* This call's own mapping: a failed munmap leaves only this process's own address space. */
  (void)munmap(stack, CKPT_GUARD_BYTES + CKPT_STACK_BYTES);
out_restore_errno:
  errno = saved_errno;
  return rc;
}

int twelve_ckpt_start(struct twelve_ckpt *c){
  int saved_errno = errno;
  assert(c != NULL);
  assert(c->state == CKPT_IDLE || c->state == CKPT_DONE);
  int state = fiber_begin(c, fiber_entry);
  errno = saved_errno;
  return state;
}

int twelve_ckpt_restart(struct twelve_ckpt *c){
  int saved_errno = errno;
  assert(c != NULL);
  assert(c->state == CKPT_IDLE || c->state == CKPT_DONE);
  int state = fiber_begin(c, restart_entry);
  errno = saved_errno;
  return state;
}

int twelve_ckpt_resume(struct twelve_ckpt *c, int sync_result){
  int saved_errno = errno;
  assert(c != NULL);
  assert(c->state == CKPT_WAIT_SYNC || c->state == CKPT_WAIT_SLICE);
  assert(sync_result <= 0);
  assert(sync_result != INT_MIN);
  c->sync_result = sync_result;
  int state = fiber_run(c);
  errno = saved_errno;
  return state;
}

int twelve_ckpt_sync_fd(const struct twelve_ckpt *c){
  assert(c != NULL);
  return c->sync_fd;
}

void twelve_ckpt_result(const struct twelve_ckpt *c, int *rc, int *log, int *ckpt){
  assert(c != NULL);
  assert(rc != NULL);
  assert(log != NULL);
  assert(ckpt != NULL);
  *rc = c->rc;
  *log = c->log;
  *ckpt = c->ckpt;
}

int twelve_ckpt_close(struct twelve_ckpt *c){
  if( !c ){
    return SQLITE_OK;
  }
  assert(c->state != CKPT_RUNNING);
  int saved_errno = errno;
  /* A suspended pass is settled here, its fsync performed inline. */
  while( c->state==CKPT_WAIT_SYNC || c->state==CKPT_WAIT_SLICE ){
    int sync_result = 0;
    if( c->state==CKPT_WAIT_SYNC ){
      while( fdatasync(c->sync_fd)!=0 ){
        int error = errno;
        if( error!=EINTR ){
          sync_result = -error;
          break;
        }
      }
    }
    /* The loop reads c->state, so the returned state adds nothing. */
    (void)twelve_ckpt_resume(c, sync_result);
  }
  int rc = sqlite3_close(c->db);
  /* This object's own mapping: a failed munmap leaves only this process's own address space. */
  (void)munmap(c->stack, CKPT_GUARD_BYTES + CKPT_STACK_BYTES);
  fault_free(c);
  sqlite3_free(c);
  errno = saved_errno;
  return rc;
}
