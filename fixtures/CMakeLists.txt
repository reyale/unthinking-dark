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

set(_flags
    -I${CMAKE_SOURCE_DIR}/engine/src -I${CMAKE_CURRENT_SOURCE_DIR}
    # After the system headers: sdk/cpp/include also holds -nostdlib stand-ins for <cstdint>
    # and <type_traits>, which must not shadow libc++'s.
    -idirafter ${CMAKE_SOURCE_DIR}/sdk/cpp/include
)
# Of the engine, a bot links only what has no World behind it (the same list is installed as
# the bot SDK: ud_install_bot_sdk in cmake/Install.cmake).
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
  ud_wasm_bot_command(${_out}
      WASI_SDK ${wasi_sdk_SOURCE_DIR}
      SOURCES ${_srcs}
      FLAGS ${_flags} "-DUD_BOT_HEADER=\"${_name}_bot.hpp\"" -DUD_BOT_CLASS=game::${_class}
      DEPENDS ${_deps}
      COMMENT "Compiling fixture ${_class} to wasm32"
  )
  list(APPEND _outputs ${_out})
endforeach()

add_custom_target(fixture_bots_wasm ALL DEPENDS ${_outputs})
install(FILES ${_outputs} DESTINATION ${UD_INSTALL_DATADIR}/bots)
