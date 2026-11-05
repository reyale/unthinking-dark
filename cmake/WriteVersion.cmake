# Run at build time by the ud_version_gen target (cmake/Version.cmake).
if(NOT VERSION)
  execute_process(COMMAND git describe --tags --always --dirty
                  WORKING_DIRECTORY ${SOURCE_DIR}
                  OUTPUT_VARIABLE VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR NOT VERSION)
    set(VERSION "unknown")
  endif()
endif()
set(text "#include \"version.hpp\"\n\nconst char* game::build_version() { return \"${VERSION}\"; }\n")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
endif()
if(NOT old STREQUAL text)
  file(WRITE ${OUT} "${text}")
endif()
