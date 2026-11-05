# The build's version string, shown by every tool's --version: `git describe --tags --always
# --dirty` (the tag if HEAD is tagged, else tag-N-g<hash>, else the bare hash; "-dirty" when the
# tree has uncommitted changes), or -DUD_VERSION=<string> to set it yourself (a release build
# from a tarball, say). It is regenerated on every build, so it follows commits without a
# reconfigure, and rewritten only when it changes, so it costs no rebuild otherwise.
set(UD_VERSION "" CACHE STRING "Version string for --version; empty: from git describe")
set(UD_VERSION_CPP ${CMAKE_BINARY_DIR}/generated/version.cpp)
add_custom_target(ud_version_gen ALL
    COMMAND ${CMAKE_COMMAND} -DOUT=${UD_VERSION_CPP} -DVERSION=${UD_VERSION}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/WriteVersion.cmake
    BYPRODUCTS ${UD_VERSION_CPP}
    COMMENT "Checking build version"
    VERBATIM
)
