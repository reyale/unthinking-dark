add_library(maps_lib STATIC map_io.cpp map_validate.cpp map_gen.cpp match_setup.cpp)
target_include_directories(maps_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${UD_INSTALL_INCLUDEDIR}>
)
# Both header-only, and no public header includes either.
target_link_libraries(maps_lib PUBLIC engine $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
                                      $<BUILD_INTERFACE:xxhash_iface>)
set_target_properties(maps_lib PROPERTIES EXPORT_NAME maps)
ud_install_library(maps_lib ${CMAKE_CURRENT_SOURCE_DIR})
install(DIRECTORY data/ DESTINATION ${UD_INSTALL_DATADIR}/maps)
