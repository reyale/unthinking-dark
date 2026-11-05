add_executable(gen_example_replay gen_replay.cpp)
target_link_libraries(gen_example_replay PRIVATE engine)
target_include_directories(gen_example_replay PRIVATE ${CMAKE_SOURCE_DIR}/fixtures)

add_executable(ud_mapgen ud_mapgen.cpp)
target_link_libraries(ud_mapgen PRIVATE maps_lib)

add_executable(ud_mapcheck ud_mapcheck.cpp)
target_link_libraries(ud_mapcheck PRIVATE maps_lib)

add_executable(ud_run ud_run.cpp)
target_link_libraries(ud_run PRIVATE runner_lib maps_lib nlohmann_json::nlohmann_json)
target_include_directories(ud_run PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
    ${CMAKE_SOURCE_DIR}/maps
    ${CMAKE_SOURCE_DIR}/runner
)

add_library(arena_lib STATIC arena.cpp)
target_link_libraries(arena_lib PUBLIC engine)
target_include_directories(arena_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/fixtures)

add_executable(ud_arena ud_arena.cpp)
target_link_libraries(ud_arena PRIVATE arena_lib)
