include(ExternalProject)
FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
    URL_HASH SHA256=36ad6e7f38540a3b21a2ac36340833f0a9e426bc1c752751c3ba669466827eae
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)
tds_add_library(sqlite STATIC server/sqlite_read.c)
target_include_directories(sqlite SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite PRIVATE SQLITE_THREADSAFE=2)
target_link_libraries(sqlite PUBLIC ${CMAKE_DL_LIBS})
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_program(MAKE_EXECUTABLE make REQUIRED)
set(URING_SOURCE "${CMAKE_BINARY_DIR}/_deps/liburing")
file(MAKE_DIRECTORY "${URING_SOURCE}/src/include")
ExternalProject_Add(uring_build
    URL https://codeload.github.com/axboe/liburing/tar.gz/refs/tags/liburing-2.15
    URL_HASH SHA256=8d052f2622dcb3678cbaee5ff582a87572672a6c0a56533cdda5b65cb636120a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${URING_SOURCE}"
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
    BUILD_COMMAND ${MAKE_EXECUTABLE} -C <SOURCE_DIR>/src liburing.a
    BUILD_BYPRODUCTS "${URING_SOURCE}/src/liburing.a"
    INSTALL_COMMAND ""
)
add_library(uring STATIC IMPORTED)
set_target_properties(uring PROPERTIES
    IMPORTED_LOCATION "${URING_SOURCE}/src/liburing.a"
    INTERFACE_INCLUDE_DIRECTORIES "${URING_SOURCE}/src/include"
)
add_dependencies(uring uring_build)
