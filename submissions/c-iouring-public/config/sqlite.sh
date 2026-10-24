# sqlite.sh — compiler flags for SQLite and the JWT engine. Sourced first by build.sh and tools/dev.sh.

# SQLite's recommended options (each measured on its own). THREADSAFE=2 is needed by the checkpoint thread
# (cursed.sh switches it to 0 for DB_CKPT=uring, which has no second connection). Five OMITs that change behaviour
# were rejected.
SQLOPTS="-DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0"
SQLOPTS="$SQLOPTS -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_USE_ALLOCA"
SQLOPTS="$SQLOPTS -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE"
SQLOPTS="$SQLOPTS -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_DECLTYPE"
if [ "$(uname -s)" = Linux ]; then
  SQLOPTS="$SQLOPTS -DHAVE_FDATASYNC=1"
fi

# JWT: the receive buffers are padded (LOOP_SLACK), and the AVX-512VL ymm engine is used where the CPU has it (exp2).
APPOPTS="-DJWT_INPUT_PADDED"
if grep -qw avx512vl /proc/cpuinfo 2> /dev/null; then
  APPOPTS="$APPOPTS -DJWT_AVX512"
fi
