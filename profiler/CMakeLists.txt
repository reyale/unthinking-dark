add_library(profiler_lib STATIC profiler.cpp)
target_include_directories(profiler_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(profiler_lib PUBLIC runner_lib)

add_executable(ud_profile ud_profile.cpp)
target_link_libraries(ud_profile PRIVATE profiler_lib)
