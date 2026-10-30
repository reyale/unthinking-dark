add_library(maps_lib STATIC map_io.cpp map_validate.cpp map_gen.cpp match_setup.cpp)
target_include_directories(maps_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(maps_lib PUBLIC engine nlohmann_json::nlohmann_json xxhash_iface)
