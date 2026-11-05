add_library(engine STATIC
    src/stats.cpp
    src/fixed.cpp
    src/rng.cpp
    src/grid.cpp
    src/world.cpp
    src/snapshot.cpp
    src/occupancy.cpp
    src/command.cpp
    src/movement.cpp
    src/combat.cpp
    src/economy.cpp
    src/territory.cpp
    src/wincheck.cpp
    src/statehash.cpp
    src/tick.cpp
    src/replay.cpp
    src/file_io.cpp
    src/replay_io.cpp
    src/frame.cpp
    src/match.cpp
    ${UD_VERSION_CPP}
)
add_dependencies(engine ud_version_gen)

target_include_directories(engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${ZSTD_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:${UD_INSTALL_INCLUDEDIR}>
)

# xxhash is header-only and no public header includes it, so installed users don't need it.
target_link_libraries(engine PUBLIC $<BUILD_INTERFACE:xxhash_iface> ${ZSTD_LIBRARIES})
set_target_properties(engine PROPERTIES EXPORT_NAME engine)
# Code built against the installed headers (inline game logic included) must keep the
# determinism flags this tree sets globally, and link what the libraries were built with.
target_compile_options(engine INTERFACE $<INSTALL_INTERFACE:-ffp-contract=off>
                                        $<INSTALL_INTERFACE:-fno-fast-math>)
if(NOT APPLE)
  # The libraries need the C++ runtime they were built with, which can be newer than the system's.
  target_link_options(engine INTERFACE $<INSTALL_INTERFACE:-static-libgcc>
                                       $<INSTALL_INTERFACE:-static-libstdc++>)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_link_options(engine INTERFACE $<INSTALL_INTERFACE:-fsanitize=address,undefined>)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(engine INTERFACE $<INSTALL_INTERFACE:-static-libasan>
                                         $<INSTALL_INTERFACE:-static-libubsan>)
  else()
    target_link_options(engine INTERFACE $<INSTALL_INTERFACE:-static-libsan>)
  endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_options(engine INTERFACE $<INSTALL_INTERFACE:-flto=thin>)
endif()
ud_install_library(engine ${CMAKE_CURRENT_SOURCE_DIR}/src)

