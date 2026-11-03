cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(twelve_dollar_drogon
        DESCRIPTION "A C++26 submission for the Twelve Dollar Challenge using Drogon and Glaze"
        VERSION 1.0.0
        LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(BUILD_CTL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_ORM OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG v1.9.13
        GIT_SHALLOW ON
)
FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v8.0.0
        GIT_SHALLOW ON
)
FetchContent_MakeAvailable(drogon glaze)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(GTest REQUIRED)

add_executable(server)

target_sources(server
        PRIVATE
        src/main.cpp
)

target_compile_options(server
        PRIVATE
        -Wall -Wextra
)
target_link_libraries(server
        PRIVATE
        Drogon::Drogon
        glaze::glaze
        OpenSSL::Crypto
        SQLite::SQLite3
)

enable_testing()
add_executable(unit_tests)

target_sources(unit_tests
        PRIVATE
        tests/jwt_test.cpp
        tests/smoke_test.cpp
)

target_include_directories(unit_tests
        PRIVATE
        src
)

target_link_libraries(unit_tests
        PRIVATE
        glaze::glaze
        OpenSSL::Crypto
        GTest::gtest_main
)

add_test(NAME unit_tests COMMAND unit_tests)
