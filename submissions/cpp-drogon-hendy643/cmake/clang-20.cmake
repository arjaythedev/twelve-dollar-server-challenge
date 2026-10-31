set(CMAKE_SYSTEM_NAME Linux)

find_program(CLANG_20 NAMES clang-20 HINTS /usr/lib/llvm-20/bin /usr/lib/llvm20/bin REQUIRED)
get_filename_component(CLANG_20_DIR "${CLANG_20}" DIRECTORY)
find_program(CLANGXX_20 NAMES clang++-20 clang++ HINTS "${CLANG_20_DIR}" NO_DEFAULT_PATH REQUIRED)
set(CMAKE_C_COMPILER "${CLANG_20}")
set(CMAKE_CXX_COMPILER "${CLANGXX_20}")

find_program(LLD_20 NAMES ld.lld-20 ld.lld HINTS /usr/lib/llvm-20/bin /usr/lib/llvm20/bin REQUIRED)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=${LLD_20}")

set(CMAKE_C_FLAGS_RELEASE_INIT "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -march=native -flto -DNDEBUG")

set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-s")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
