# The runner supplies bot linear memory itself (to enforce MEMORY_CAP_MB) using
# mmap/mprotect, and is only built and tested on Linux. Everything that runs .wasm
# bots (ud_run, the profiler, runner/profiler tests) depends on it.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "runner/ is Linux-only (capped bot memory uses mmap/mprotect); "
                      "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'")
endif()

add_library(runner_lib STATIC wasm_bot.cpp)

target_include_directories(runner_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${UD_INSTALL_INCLUDEDIR}>
)

# Installed, libwasmtime.a goes alongside and ai_gameConfig.cmake defines ai_game::wasmtime.
target_link_libraries(runner_lib PUBLIC engine $<BUILD_INTERFACE:wasmtime_lib>
                                        $<INSTALL_INTERFACE:ai_game::wasmtime>)
set_target_properties(runner_lib PROPERTIES EXPORT_NAME runner)
ud_install_library(runner_lib ${CMAKE_CURRENT_SOURCE_DIR})
install(FILES ${wasmtime_SOURCE_DIR}/lib/libwasmtime.a DESTINATION ${UD_INSTALL_LIBDIR})
