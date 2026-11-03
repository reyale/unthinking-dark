# Every fixture bot also builds as a WASM module from its unchanged header, through
# sdk/cpp/include/ud/bot_host.hpp: a fixture is written against game::Bot, the same API a
# hosted bot gets. FixtureWasm.* (tests/test_fixture_wasm.cpp) checks each module plays
# exactly as its native build. To add a fixture: <name>_bot.hpp defining game::<Class>, an
# entry here, and one in that test.
set(UD_FIXTURE_BOTS
    idle:IdleBot
    rush:RushBot
    econ:EconBot
    turtle:TurtleBot
    expander:ExpanderBot
    adaptive:AdaptiveBot
)

if(NOT UD_WASM_FIXTURES)
  return()
endif()

set(FIXTURE_WASM_DIR ${CMAKE_BINARY_DIR}/fixtures_wasm)
set(FIXTURE_WASM_DIR ${FIXTURE_WASM_DIR} PARENT_SCOPE)

set(_wasi ${wasi_sdk_SOURCE_DIR})
set(_flags
    --target=wasm32-wasip1 --sysroot=${_wasi}/share/wasi-sysroot
    -std=c++23 -O2 -DNDEBUG -fno-exceptions -fno-rtti
    -ffp-contract=off -fno-fast-math -mno-bulk-memory
    -I${CMAKE_SOURCE_DIR}/engine/src -I${CMAKE_CURRENT_SOURCE_DIR}
    # After the system headers: sdk/cpp/include also holds -nostdlib stand-ins for <cstdint>
    # and <type_traits>, which must not shadow libc++'s.
    -idirafter ${CMAKE_SOURCE_DIR}/sdk/cpp/include
)
set(_link
    -Wl,--no-entry -Wl,--strip-all -Wl,--export-memory
    -Wl,--export=abi_version -Wl,--export=init -Wl,--export=on_tick
    -Wl,--export=SNAPSHOT_ADDR -Wl,--export=COMMAND_ADDR
)
# Of the engine, a bot links only what has no World behind it.
set(_srcs
    ${CMAKE_CURRENT_SOURCE_DIR}/wasm_main.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/grid.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/stats.cpp
    ${CMAKE_SOURCE_DIR}/sdk/cpp/runtime.cpp
    ${CMAKE_SOURCE_DIR}/sdk/cpp/wasi_stubs.cpp
)
file(GLOB _deps
    ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp
    ${CMAKE_SOURCE_DIR}/engine/src/*.hpp
    ${CMAKE_SOURCE_DIR}/sdk/cpp/include/ud/*.hpp
)

set(_outputs)
foreach(_entry ${UD_FIXTURE_BOTS})
  string(REPLACE ":" ";" _parts ${_entry})
  list(GET _parts 0 _name)
  list(GET _parts 1 _class)
  set(_out ${FIXTURE_WASM_DIR}/${_name}.wasm)
  add_custom_command(
      OUTPUT ${_out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${FIXTURE_WASM_DIR}
      COMMAND ${_wasi}/bin/clang++ ${_flags}
              "-DUD_BOT_HEADER=\"${_name}_bot.hpp\"" -DUD_BOT_CLASS=game::${_class}
              ${_srcs} ${_link} -o ${_out}
      DEPENDS ${_srcs} ${_deps}
      COMMENT "Compiling fixture ${_class} to wasm32"
      VERBATIM
  )
  list(APPEND _outputs ${_out})
endforeach()

add_custom_target(fixture_bots_wasm ALL DEPENDS ${_outputs})
