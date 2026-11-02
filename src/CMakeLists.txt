cmake_minimum_required(VERSION 3.25)
project(twelve_dollar_server VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The server targets Linux. Use the Docker build on Windows.")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(TDS_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)

include(ExternalProject)
include(FetchContent)
set(LLHTTP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLHTTP_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(llhttp
    URL https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.4.3
    URL_HASH SHA256=1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(llhttp)
FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
    URL_HASH SHA256=36ad6e7f38540a3b21a2ac36340833f0a9e426bc1c752751c3ba669466827eae
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(yyjson_source
    URL https://codeload.github.com/ibireme/yyjson/tar.gz/refs/tags/0.13.0
    URL_HASH SHA256=34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae
    SOURCE_SUBDIR no-cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite yyjson_source)
add_library(sqlite STATIC server/sqlite_read.c)
target_include_directories(sqlite SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite PRIVATE SQLITE_THREADSAFE=2)
target_link_libraries(sqlite PUBLIC ${CMAKE_DL_LIBS})
add_library(yyjson STATIC ${yyjson_source_SOURCE_DIR}/src/yyjson.c)
target_include_directories(yyjson SYSTEM PUBLIC ${yyjson_source_SOURCE_DIR}/src)
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

add_library(parsing STATIC server/parsing.cpp)
target_include_directories(parsing PUBLIC server)
add_library(transport STATIC
    server/transport.cpp
    server/transport/http_parser.cpp
    server/transport/connection.cpp
    server/transport/ring.cpp
)
target_link_libraries(transport PRIVATE uring llhttp_static parsing)
target_include_directories(transport PUBLIC server)
add_library(api STATIC server/api.cpp server/auth.cpp server/database.cpp server/json.cpp)
target_link_libraries(api PRIVATE sqlite yyjson OpenSSL::Crypto parsing Threads::Threads)
target_include_directories(api PUBLIC server)
add_executable(server server/main.cpp)
target_link_libraries(server PRIVATE transport api uring sqlite)
add_executable(transport_tests checks/transport.cpp)
target_link_libraries(transport_tests PRIVATE transport uring)
add_executable(checkpoint_tests checks/checkpoint.cpp)
target_link_libraries(checkpoint_tests PRIVATE api sqlite)
target_link_options(checkpoint_tests PRIVATE
    -Wl,--wrap=sqlite3_open_v2 -Wl,--wrap=sqlite3_close -Wl,--wrap=sqlite3_wal_checkpoint_v2)
add_executable(load_generator load/main.cpp)
target_link_libraries(load_generator PRIVATE llhttp_static yyjson)
foreach(target server transport transport_tests checkpoint_tests api parsing sqlite load_generator)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    if(TDS_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

enable_testing()
add_test(NAME io_uring_operations COMMAND server --self-test)
add_test(NAME transport_lifetimes COMMAND transport_tests)
add_test(NAME checkpoint_lifetimes COMMAND checkpoint_tests)
set_tests_properties(checkpoint_lifetimes PROPERTIES TIMEOUT 15)
add_test(NAME load_generator_conformance COMMAND load_generator --self-test)
