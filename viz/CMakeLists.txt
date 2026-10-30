add_library(viz_lib STATIC render.cpp ascii_out.cpp)
target_link_libraries(viz_lib PUBLIC engine)
target_include_directories(viz_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ud_viz main.cpp)
target_link_libraries(ud_viz PRIVATE viz_lib)

find_package(Curses QUIET)
if(CURSES_FOUND)
  add_library(ncurses_out STATIC ncurses_out.cpp)
  target_link_libraries(ncurses_out PUBLIC viz_lib ${CURSES_LIBRARIES})
  target_include_directories(ncurses_out PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CURSES_INCLUDE_DIRS})

  add_executable(ud_viz_nc main_nc.cpp)
  target_link_libraries(ud_viz_nc PRIVATE ncurses_out)
else()
  message(STATUS "ncurses not found — ud_viz_nc will not be built")
endif()

# ---- Optional: Dear ImGui visualizer ----------------------------------------
# Requires OpenGL and GLFW3 (e.g. apt install libglfw3-dev on Ubuntu).
# Dear ImGui is fetched automatically via FetchContent.
find_package(OpenGL QUIET)
find_package(glfw3 3.3 QUIET)

if(OpenGL_FOUND AND glfw3_FOUND)
  message(STATUS "Found OpenGL and GLFW — building ud_viz_imgui")

  include(FetchContent)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6   # any >= 1.87 should work; bump as desired
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(imgui)

  add_library(ud_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(ud_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(ud_imgui PUBLIC glfw OpenGL::GL)
  target_compile_options(ud_imgui PRIVATE -w)  # imgui ships with minor warnings

  # Baked-in default so the apps find the repo's shapes from any working directory;
  # override per run with --shapes, or per build with -DUD_SHAPES_FILE=<path>.
  set(UD_SHAPES_FILE "${CMAKE_SOURCE_DIR}/shapes.udshapes" CACHE FILEPATH
      "Default shapes file for ud_viz_imgui and ud_shape_editor")

  add_executable(ud_viz_imgui imgui_main.cpp)
  target_include_directories(ud_viz_imgui PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
  )
  target_link_libraries(ud_viz_imgui PRIVATE engine ud_imgui)
  target_compile_definitions(ud_viz_imgui PRIVATE UD_DEFAULT_SHAPES_PATH="${UD_SHAPES_FILE}")

  add_executable(ud_shape_editor shape_editor_main.cpp)
  target_include_directories(ud_shape_editor PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
  )
  target_link_libraries(ud_shape_editor PRIVATE engine ud_imgui)
  target_compile_definitions(ud_shape_editor PRIVATE UD_DEFAULT_SHAPES_PATH="${UD_SHAPES_FILE}")

else()
  message(STATUS "OpenGL or GLFW not found — skipping ud_viz_imgui (apt install libglfw3-dev)")
endif()
