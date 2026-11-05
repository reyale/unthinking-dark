# How a bot is compiled to a WASM module, shared by the fixture bots (fixtures/CMakeLists.txt) and,
# installed, by ai_game_add_bot() in projects outside this repo, so every bot gets the same line.
# -mno-bulk-memory matters most: the runner rejects modules using memory.copy/memory.fill.
#
#   ud_wasm_bot_command(<out.wasm> WASI_SDK <dir> SOURCES ... [FLAGS ...] [DEPENDS ...]
#                       [COMMENT text])
function(ud_wasm_bot_command out)
  cmake_parse_arguments(PARSE_ARGV 1 BOT "" "WASI_SDK;COMMENT" "SOURCES;FLAGS;DEPENDS")
  get_filename_component(_dir ${out} DIRECTORY)
  add_custom_command(
      OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}
      COMMAND ${BOT_WASI_SDK}/bin/clang++
              --target=wasm32-wasip1 --sysroot=${BOT_WASI_SDK}/share/wasi-sysroot
              -std=c++23 -O2 -DNDEBUG -fno-exceptions -fno-rtti
              -ffp-contract=off -fno-fast-math -mno-bulk-memory
              ${BOT_FLAGS} ${BOT_SOURCES}
              -Wl,--no-entry -Wl,--strip-all -Wl,--export-memory
              -Wl,--export=abi_version -Wl,--export=init -Wl,--export=on_tick
              -Wl,--export=SNAPSHOT_ADDR -Wl,--export=COMMAND_ADDR
              -o ${out}
      DEPENDS ${BOT_SOURCES} ${BOT_DEPENDS}
      COMMENT "${BOT_COMMENT}"
      VERBATIM
  )
endfunction()
