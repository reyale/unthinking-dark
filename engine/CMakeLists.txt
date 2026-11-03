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
)

target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ZSTD_INCLUDE_DIRS}
)

target_link_libraries(engine PUBLIC xxhash_iface ${ZSTD_LIBRARIES})

