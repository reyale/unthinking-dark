# The runner supplies bot linear memory itself (to enforce MEMORY_CAP_MB) using
# mmap/mprotect, and is only built and tested on Linux. Everything that runs .wasm
# bots (ud_run, the profiler, runner/profiler tests) depends on it.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "runner/ is Linux-only (capped bot memory uses mmap/mprotect); "
                      "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'")
endif()

add_library(runner_lib STATIC wasm_bot.cpp)

target_include_directories(runner_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(runner_lib PUBLIC engine wasmtime_lib)
