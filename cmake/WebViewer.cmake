# UD_WEB_VIEWER (on by default for Release) adds the `web_viewer` target: the browser replay
# player (viz/Makefile.em) built into viz/web/ as part of `all`, and installed to
# share/ai_game/web/ (AI_GAME_WEB_DIR), with everything it needs fetched at configure time,
# like wasi-sdk:
#   - a pinned emsdk (~1.6 GB installed, once per build dir), or UD_EMSDK to reuse one that
#     has Emscripten ${UD_EMSDK_VERSION} installed and activated;
#   - a Python >= 3.10 for emcc (Ubuntu 20.04 ships 3.8): the system's if new enough, else
#     uv's (`uv python install`), else UD_PYTHON;
#   - Dear ImGui (viz/CMakeLists.txt) and xxHash (top level), as source only.

set(UD_EMSDK_VERSION 7.0.1)
set(UD_EMSDK_URL https://github.com/emscripten-core/emsdk/archive/refs/tags/${UD_EMSDK_VERSION}.tar.gz)
set(UD_EMSDK_SHA256 bfd13deac54e4cbefac37aec0277a24f46c33823c17004d99caa62b37f122bf3)
set(UD_EMSDK "" CACHE PATH
    "An emsdk with Emscripten ${UD_EMSDK_VERSION} activated; empty fetches and installs one")
set(UD_PYTHON "" CACHE FILEPATH "A Python >= 3.10 for emcc; empty finds one or fetches it with uv")
set(UD_PYTHON_FETCH_VERSION 3.12)

function(_ud_web_python out)
  if(UD_PYTHON)
    set(${out} ${UD_PYTHON} PARENT_SCOPE)
    return()
  endif()
  find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    set(${out} ${Python3_EXECUTABLE} PARENT_SCOPE)
    return()
  endif()
  find_program(UD_UV uv HINTS $ENV{HOME}/.local/bin $ENV{HOME}/.cargo/bin)
  if(NOT UD_UV)
    message(FATAL_ERROR "UD_WEB_VIEWER: emcc needs Python >= 3.10 and none was found. "
                        "Install uv (https://docs.astral.sh/uv/) or set -DUD_PYTHON=<python>.")
  endif()
  # Run outside the source tree, so uv doesn't read a pyproject.toml it finds above it.
  set(_find ${UD_UV} python find --no-project ">=3.10")
  execute_process(COMMAND ${_find} WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                  OUTPUT_VARIABLE _py OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc
                  ERROR_QUIET)
  if(NOT _rc EQUAL 0)
    message(STATUS "UD_WEB_VIEWER: fetching Python ${UD_PYTHON_FETCH_VERSION} with uv")
    execute_process(COMMAND ${UD_UV} python install ${UD_PYTHON_FETCH_VERSION}
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND ${_find} WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    OUTPUT_VARIABLE _py OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
  endif()
  set(${out} ${_py} PARENT_SCOPE)
endfunction()

function(_ud_web_emsdk out python)
  if(UD_EMSDK)
    set(_dir ${UD_EMSDK})
  else()
    include(FetchContent)
    FetchContent_Declare(emsdk URL ${UD_EMSDK_URL} URL_HASH SHA256=${UD_EMSDK_SHA256})
    FetchContent_MakeAvailable(emsdk)
    set(_dir ${emsdk_SOURCE_DIR})
  endif()
  set(_stamp ${_dir}/upstream/emscripten/emscripten-version.txt)
  set(_have "")
  if(EXISTS ${_stamp})
    file(READ ${_stamp} _have)
    string(STRIP "${_have}" _have)
  endif()
  if(NOT _have STREQUAL "\"${UD_EMSDK_VERSION}\"")
    if(UD_EMSDK)
      message(FATAL_ERROR "UD_WEB_VIEWER: ${UD_EMSDK} has Emscripten ${_have}, not "
                          "${UD_EMSDK_VERSION}. Run `emsdk install ${UD_EMSDK_VERSION} && "
                          "emsdk activate ${UD_EMSDK_VERSION}` there, or leave UD_EMSDK empty.")
    endif()
    message(STATUS "UD_WEB_VIEWER: installing Emscripten ${UD_EMSDK_VERSION} (~1.6 GB, once)")
    # `activate` writes the emsdk's own .emscripten config, which em++ reads; it doesn't touch
    # the shell profile.
    foreach(_step install activate)
      execute_process(COMMAND ${CMAKE_COMMAND} -E env EMSDK_PYTHON=${python}
                              ${_dir}/emsdk ${_step} ${UD_EMSDK_VERSION}
                      WORKING_DIRECTORY ${_dir} COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
  endif()
  set(${out} ${_dir} PARENT_SCOPE)
endfunction()

# Called from viz/CMakeLists.txt once Dear ImGui is populated.
function(ud_add_web_viewer imgui_dir)
  _ud_web_python(_py)
  _ud_web_emsdk(_emsdk ${_py})
  find_program(UD_MAKE make REQUIRED)
  message(STATUS "UD_WEB_VIEWER: emsdk ${_emsdk}, Python ${_py}")
  add_custom_target(web_viewer ALL
    COMMAND ${UD_MAKE} -f Makefile.em
            EMSDK_DIR=${_emsdk} PYTHON310=${_py}
            IMGUI_DIR=${imgui_dir} XXHASH_DIR=${xxhash_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/viz
    COMMENT "Building the browser replay player into viz/web/"
    USES_TERMINAL)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/viz/web/ DESTINATION ${UD_INSTALL_DATADIR}/web)
endfunction()
