include(FetchContent)
set(LLHTTP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLHTTP_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(llhttp
    URL https://codeload.github.com/nodejs/llhttp/tar.gz/refs/tags/release/v9.4.3
    URL_HASH SHA256=1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(llhttp)
FetchContent_Declare(yyjson_source
    URL https://codeload.github.com/ibireme/yyjson/tar.gz/refs/tags/0.13.0
    URL_HASH SHA256=34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae
    SOURCE_SUBDIR no-cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(yyjson_source)
add_library(yyjson STATIC ${yyjson_source_SOURCE_DIR}/src/yyjson.c)
target_include_directories(yyjson SYSTEM PUBLIC ${yyjson_source_SOURCE_DIR}/src)
