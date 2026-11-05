# Installed with the package (ai_gameConfig.cmake includes it): builds a bot outside this repo.
#
#   ai_game_add_bot(<name> SOURCES main.cpp ... [OUTPUT <file.wasm>])
#
# One source defines the module, from a class written against game::Bot:
#
#   #include "my_bot.hpp"
#   #include <ud/bot_host.hpp>
#   UD_WASM_BOT(my::Bot)
#
# Adds target <name>, building <name>.wasm in the current binary dir (or OUTPUT), whose path
# is the target's WASM_FILE property. Needs wasi-sdk: set AI_GAME_WASI_SDK to an unpacked
# wasi-sdk 24 (the version the engine is built against; from LLVM 19 clang emits bulk-memory
# instructions by default, which the runner rejects), or leave it empty to download it once
# per build directory.
include(${CMAKE_CURRENT_LIST_DIR}/WasmBot.cmake)

set(AI_GAME_WASI_SDK "" CACHE PATH "An unpacked wasi-sdk 24; empty downloads it")

function(_ai_game_wasi_sdk out)
  if(AI_GAME_WASI_SDK)
    set(${out} ${AI_GAME_WASI_SDK} PARENT_SCOPE)
    return()
  endif()
  include(FetchContent)
  FetchContent_Declare(ai_game_wasi_sdk URL ${AI_GAME_WASI_SDK_URL}
                       URL_HASH SHA256=${AI_GAME_WASI_SDK_SHA256})
  FetchContent_MakeAvailable(ai_game_wasi_sdk)
  set(${out} ${ai_game_wasi_sdk_SOURCE_DIR} PARENT_SCOPE)
endfunction()

function(ai_game_add_bot name)
  cmake_parse_arguments(PARSE_ARGV 1 BOT "" "OUTPUT" "SOURCES")
  if(NOT BOT_SOURCES)
    message(FATAL_ERROR "ai_game_add_bot(${name}): no SOURCES")
  endif()
  if(NOT BOT_OUTPUT)
    set(BOT_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.wasm)
  endif()
  _ai_game_wasi_sdk(_wasi)

  set(_srcs)
  set(_deps)
  foreach(_src ${BOT_SOURCES})
    get_filename_component(_abs ${_src} ABSOLUTE)
    list(APPEND _srcs ${_abs})
    # One clang++ call compiles every source, so there is no depfile: rebuild on any header
    # beside a source.
    get_filename_component(_dir ${_abs} DIRECTORY)
    file(GLOB_RECURSE _hdrs CONFIGURE_DEPENDS ${_dir}/*.hpp ${_dir}/*.h)
    list(APPEND _deps ${_hdrs})
  endforeach()
  file(GLOB _sdk_srcs ${AI_GAME_SDK_DIR}/*.cpp)
  file(GLOB_RECURSE _sdk_hdrs ${AI_GAME_INCLUDE_DIR}/*.hpp)

  ud_wasm_bot_command(${BOT_OUTPUT}
      WASI_SDK ${_wasi}
      SOURCES ${_srcs} ${_sdk_srcs}
      FLAGS -I${AI_GAME_INCLUDE_DIR}
      DEPENDS ${_deps} ${_sdk_hdrs}
      COMMENT "Compiling bot ${name} to wasm32"
  )
  add_custom_target(${name} ALL DEPENDS ${BOT_OUTPUT})
  set_target_properties(${name} PROPERTIES WASM_FILE ${BOT_OUTPUT})
endfunction()
