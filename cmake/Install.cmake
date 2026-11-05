# `cmake --install build --prefix <dir>` installs the engine, map and WASM-runner libraries with
# their headers, a CMake package (find_package(ai_game) gives ai_game::engine, ai_game::maps and
# ai_game::runner), the CLI tools, the maps, the fixture bots built as .wasm, and the bot SDK
# (ai_game_add_bot(), cmake/ai_gameBot.cmake). For projects outside this repo: the tournament
# harness, and bots.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Headers are included by bare name ("match.hpp"), so all three libraries share one directory.
set(UD_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/ai_game)
set(UD_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR}/ai_game)
set(UD_INSTALL_DATADIR ${CMAKE_INSTALL_DATADIR}/ai_game)
set(UD_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/ai_game)

# Position-independent, so a user can link them into a shared object too (a Python module).
function(ud_install_library target header_dir)
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  install(TARGETS ${target} EXPORT ai_game_targets ARCHIVE DESTINATION ${UD_INSTALL_LIBDIR})
  file(GLOB _headers ${header_dir}/*.hpp)
  install(FILES ${_headers} DESTINATION ${UD_INSTALL_INCLUDEDIR})
endfunction()

# What a bot compiles besides its own sources: the host glue (ud/*.hpp, next to the engine's
# headers), and the sources fixtures/CMakeLists.txt links into every fixture bot.
function(ud_install_bot_sdk)
  file(GLOB _ud ${PROJECT_SOURCE_DIR}/sdk/cpp/include/ud/*.hpp)
  install(FILES ${_ud} DESTINATION ${UD_INSTALL_INCLUDEDIR}/ud)
  install(FILES ${PROJECT_SOURCE_DIR}/engine/src/grid.cpp ${PROJECT_SOURCE_DIR}/engine/src/stats.cpp
                ${PROJECT_SOURCE_DIR}/sdk/cpp/runtime.cpp ${PROJECT_SOURCE_DIR}/sdk/cpp/wasi_stubs.cpp
          DESTINATION ${UD_INSTALL_DATADIR}/sdk)
  install(FILES ${PROJECT_SOURCE_DIR}/cmake/WasmBot.cmake ${PROJECT_SOURCE_DIR}/cmake/ai_gameBot.cmake
          DESTINATION ${UD_INSTALL_CMAKEDIR})
endfunction()

function(ud_install_package)
  ud_install_bot_sdk()
  foreach(_tool ${ARGN})
    if(TARGET ${_tool})
      install(TARGETS ${_tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
  endforeach()
  install(EXPORT ai_game_targets NAMESPACE ai_game:: FILE ai_gameTargets.cmake
          DESTINATION ${UD_INSTALL_CMAKEDIR})
  configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/ai_gameConfig.cmake.in
      ${PROJECT_BINARY_DIR}/ai_gameConfig.cmake INSTALL_DESTINATION ${UD_INSTALL_CMAKEDIR})
  # The engine's ABI and replay format change between versions, so only an exact match will do.
  write_basic_package_version_file(${PROJECT_BINARY_DIR}/ai_gameConfigVersion.cmake
      COMPATIBILITY ExactVersion)
  install(FILES ${PROJECT_BINARY_DIR}/ai_gameConfig.cmake
                ${PROJECT_BINARY_DIR}/ai_gameConfigVersion.cmake
          DESTINATION ${UD_INSTALL_CMAKEDIR})
endfunction()
