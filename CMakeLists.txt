cmake_minimum_required(VERSION 3.16)

# Register src/mesh as an additional component directory so ESP-IDF picks
# up the portable mesh core (component name "mesh", see
# src/mesh/CMakeLists.txt) alongside the usual main/ and components/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src/mesh")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espnow-mesh)
