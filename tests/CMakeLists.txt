add_executable(engine_tests
    test_rng.cpp
    test_ids.cpp
    test_world.cpp
    test_determinism.cpp
    test_combat.cpp
    test_territory.cpp
    test_renderer.cpp
    test_bots.cpp
    test_file_io.cpp
)

target_include_directories(engine_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
    ${CMAKE_SOURCE_DIR}/fixtures
    ${CMAKE_SOURCE_DIR}/viz
)

target_link_libraries(engine_tests PRIVATE engine viz_lib GTest::gtest_main)

add_executable(runner_tests
    test_wasm_runner.cpp
)

target_include_directories(runner_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
    ${CMAKE_SOURCE_DIR}/runner
)

target_link_libraries(runner_tests PRIVATE runner_lib GTest::gtest_main)
target_compile_definitions(runner_tests PRIVATE
    SDK_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/sdk/cpp/examples"
)

add_executable(profiler_tests test_profiler.cpp)
target_link_libraries(profiler_tests PRIVATE profiler_lib GTest::gtest_main)
target_compile_definitions(profiler_tests PRIVATE
    SDK_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/sdk/cpp/examples"
)

# ---- SDK wasm bot compilation ------------------------------------------------
# Compiles sdk/cpp/examples/*.cpp → *.wasm via clang++-18 as part of the normal
# build. runner_tests depends on these outputs so they are always fresh before
# any test runs. If clang++-18 is absent the wasm files are not built and the
# Sdk* tests will skip with a clear message.

find_program(CLANGPP18 clang++-18)
if(CLANGPP18)
  set(_SDK ${CMAKE_SOURCE_DIR}/sdk/cpp)
  set(_EX  ${_SDK}/examples)

  add_custom_command(
    OUTPUT
      ${_EX}/rush_bot.wasm
      ${_EX}/idle_bot.wasm
      ${_EX}/quadratic_bot.wasm
      ${_EX}/leaky_bot.wasm
      ${_EX}/copy_bot.wasm
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${_SDK} examples
    DEPENDS
      ${_EX}/rush_bot.cpp
      ${_EX}/idle_bot.cpp
      ${_EX}/quadratic_bot.cpp
      ${_EX}/leaky_bot.cpp
      ${_EX}/copy_bot.cpp
      ${_SDK}/runtime.cpp
      ${_SDK}/Makefile
      ${_SDK}/include/ud/types.hpp
      ${_SDK}/include/cstddef
      ${_SDK}/include/cstdint
      ${_SDK}/include/type_traits
      ${CMAKE_SOURCE_DIR}/engine/src/abi.hpp
      ${CMAKE_SOURCE_DIR}/engine/src/config.hpp
    COMMENT "Compiling SDK bot examples to wasm32"
    VERBATIM
  )

  add_custom_target(sdk_bots_wasm DEPENDS
    ${_EX}/rush_bot.wasm
    ${_EX}/idle_bot.wasm
    ${_EX}/quadratic_bot.wasm
    ${_EX}/leaky_bot.wasm
    ${_EX}/copy_bot.wasm
  )

  add_dependencies(runner_tests sdk_bots_wasm)
  add_dependencies(profiler_tests sdk_bots_wasm)
else()
  message(WARNING "clang++-18 not found — SDK wasm bots will not be compiled; WasmBot.Sdk* tests will skip")
endif()

add_executable(map_tests test_map_io.cpp test_match_setup.cpp test_turtle.cpp test_adaptive.cpp)
target_include_directories(map_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
    ${CMAKE_SOURCE_DIR}/maps
    ${CMAKE_SOURCE_DIR}/fixtures
)
target_link_libraries(map_tests PRIVATE maps_lib GTest::gtest_main)
target_compile_definitions(map_tests PRIVATE
    MAPS_DATA_DIR="${CMAKE_SOURCE_DIR}/maps/data"
)

include(GoogleTest)
gtest_discover_tests(engine_tests)
gtest_discover_tests(runner_tests)
gtest_discover_tests(map_tests)
gtest_discover_tests(profiler_tests)
