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

# JWT (cursed-jwt): the receive buffers are padded (LOOP_SLACK). The library picks its SHA-256 engine at run time
# (AVX-512VL, AVX2, ARMv8 SHA2, NEON or scalar); on x86-64 Linux, single tokens use OpenSSL's block function.
APPOPTS="-DJWT_INPUT_PADDED"
JWT_ASM=""
if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ]; then
  APPOPTS="$APPOPTS -DJWT_OSSL_SHA256"
  JWT_ASM=third_party/cursed-jwt/src/sha256_x86_64_ossl.S
fi
