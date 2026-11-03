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
set(TDS_TELEMETRY "off" CACHE STRING "Telemetry collection: off, basic or profile")
set_property(CACHE TDS_TELEMETRY PROPERTY STRINGS off basic profile)
set(telemetry_modes off basic profile)
list(FIND telemetry_modes "${TDS_TELEMETRY}" telemetry_level)
if(telemetry_level EQUAL -1)
    message(FATAL_ERROR "TDS_TELEMETRY must be off, basic or profile")
endif()

include(cmake/targets.cmake)
include(cmake/dependencies.cmake)

tds_add_library(parsing STATIC server/parsing.cpp)
target_include_directories(parsing PUBLIC server)
tds_add_library(transport STATIC
    server/server.cpp
    server/http/parser.cpp
    server/http/session.cpp
    server/http/reply.cpp
    server/net/buffers.cpp
    server/net/connections.cpp
    server/net/ring.cpp
    server/net/reactor.cpp
)
target_link_libraries(transport PUBLIC uring llhttp_static PRIVATE api parsing)
target_include_directories(transport PUBLIC server)
tds_add_library(api STATIC server/app/application.cpp server/auth.cpp server/storage/database.cpp server/json.cpp
    server/app/post_writer.cpp)
target_link_libraries(api PRIVATE sqlite yyjson OpenSSL::Crypto parsing Threads::Threads)
target_include_directories(api PUBLIC server)
set(sync_wrap_options -Wl,--wrap=fsync -Wl,--wrap=fdatasync)
target_compile_definitions(sqlite PRIVATE TDS_TELEMETRY_LEVEL=${telemetry_level})
target_compile_definitions(api PRIVATE TDS_TELEMETRY_LEVEL=${telemetry_level})
target_compile_definitions(transport PUBLIC TDS_TELEMETRY_LEVEL=${telemetry_level})

tds_add_library(telemetry_output STATIC EXCLUDE_FROM_ALL server/telemetry/output.cpp)
target_include_directories(telemetry_output PUBLIC server)
target_link_libraries(telemetry_output PRIVATE yyjson Threads::Threads)
if(telemetry_level GREATER 0)
    target_sources(api PRIVATE server/diagnostics/database.cpp)
    target_sources(transport PRIVATE server/diagnostics/io.cpp)
    target_link_libraries(api PRIVATE telemetry_output)
    target_link_libraries(transport PRIVATE telemetry_output)
endif()
if(telemetry_level EQUAL 2)
    tds_add_library(sync_hooks OBJECT server/sync_timing.cpp)
    target_compile_definitions(sync_hooks PRIVATE TDS_TELEMETRY_LEVEL=2)
    # Linker wrapping discovers these symbols after scanning API archives, so include the object directly
    target_link_libraries(api INTERFACE $<TARGET_OBJECTS:sync_hooks>)
    target_link_options(api INTERFACE ${sync_wrap_options})
endif()
tds_add_executable(server server/main.cpp)
target_link_libraries(server PRIVATE transport api uring sqlite)
tds_add_executable(load_generator load/main.cpp)
target_link_libraries(load_generator PRIVATE llhttp_static yyjson)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
add_subdirectory(benchmarks)
