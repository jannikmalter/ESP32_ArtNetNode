# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# index.html is embedded into flash by PlatformIO via `board_build.embed_txtfiles`
# in platformio.ini (the CMake EMBED_TXTFILES rule isn't run by PlatformIO's
# SCons build), and served by the web UI — see start_webserver in main.c.
idf_component_register(SRCS ${app_sources})
