# Profiles each fixture bot with ud_profile, every scenario, and fails if any forfeits (out of
# fuel, a trap, or past the memory cap: ud_profile exits non-zero). Run before `cmake --install`
# copies anything (top-level CMakeLists.txt) and by the `validate_bots` target, so an install
# never ships a bot that loses a ranked match on the budgets alone.
#
# Script mode: PROFILER (ud_profile) and BOTS (the .wasm files) are set by the generated
# validate_bots.cmake (fixtures/CMakeLists.txt) that includes this.

set(_failed)
foreach(_bot ${BOTS})
  get_filename_component(_name ${_bot} NAME_WE)
  if(NOT EXISTS ${_bot})
    message(FATAL_ERROR "validate_bots: ${_bot} is missing: build first")
  endif()
  execute_process(COMMAND ${PROFILER} ${_bot} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out
                  ERROR_VARIABLE _out)
  if(_rc EQUAL 0)
    message(STATUS "validate_bots: ${_name} ok")
  else()
    message("${_out}")
    message(STATUS "validate_bots: ${_name} FAILED (ud_profile exit ${_rc})")
    list(APPEND _failed ${_name})
  endif()
endforeach()
if(_failed)
  list(JOIN _failed ", " _failed)
  message(FATAL_ERROR "validate_bots: ${_failed} failed the profiler; nothing was installed. "
                      "Configure with -DUD_VALIDATE_INSTALL=OFF to install anyway.")
endif()
