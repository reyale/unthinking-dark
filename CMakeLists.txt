cmake_minimum_required(VERSION 3.20)

project(ai_game
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerFlags.cmake)
include(cmake/Coverage.cmake)

# ---------------------------------------------------------------------------
# Dependencies via FetchContent and pkg-config
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# gtest v1.14 rejects CXX_STANDARD 23 inside its cmake; build it at 20 (ABI-compatible).
set(CMAKE_CXX_STANDARD 20)
FetchContent_MakeAvailable(googletest)
set(CMAKE_CXX_STANDARD 23)

FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)
FetchContent_MakeAvailable(xxhash)

# xxhash is header-only when XXH_INLINE_ALL is defined.
add_library(xxhash_iface INTERFACE)
target_include_directories(xxhash_iface INTERFACE ${xxhash_SOURCE_DIR})
target_compile_definitions(xxhash_iface INTERFACE XXH_INLINE_ALL)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# Wasmtime C API (prebuilt tarball, x86_64 Linux)
# NOTE: This tarball is fetched from GitHub releases. For a self-hosted build
# that doesn't depend on GitHub availability, mirror the tarball to your own
# storage (S3, GCS, internal server) and update the URL below. The SHA256
# must remain the same — it's your integrity guarantee regardless of source.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    wasmtime
    URL      https://github.com/bytecodealliance/wasmtime/releases/download/v45.0.1/wasmtime-v45.0.1-x86_64-linux-c-api.tar.xz
    URL_HASH SHA256=641417c7b86324e3dd986cebd33da68e88963f2b7707a86551c366f178086d79
)
FetchContent_MakeAvailable(wasmtime)

add_library(wasmtime_lib STATIC IMPORTED GLOBAL)
set_target_properties(wasmtime_lib PROPERTIES
    IMPORTED_LOCATION             "${wasmtime_SOURCE_DIR}/lib/libwasmtime.a"
    INTERFACE_INCLUDE_DIRECTORIES "${wasmtime_SOURCE_DIR}/include"
)
# libwasmtime.a requires these system libs on Linux
target_link_libraries(wasmtime_lib INTERFACE dl m pthread)

add_subdirectory(engine)
add_subdirectory(maps)
add_subdirectory(viz)
add_subdirectory(tools)
add_subdirectory(runner)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
