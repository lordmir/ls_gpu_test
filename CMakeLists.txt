cmake_minimum_required(VERSION 3.28)
project(gpu_demo)

set(CMAKE_CXX_STANDARD 20)

option(LANDSTALKER_BUILD_SHARED "Build liblandstalker as a shared library" OFF)
option(INSTALL_DEPS "Automatically download and build dependencies" ON)

include(dependencies.cmake)

# Find/Install dependencies
InstallDependencies()

# Find OpenGL
find_package(OpenGL REQUIRED)

# Add liblandstalker submodule
add_subdirectory(modules/liblandstalker)

# Source files
set(SOURCES
    main.cpp
    MainFrame.cpp
    GLCanvas.cpp
    MapRenderer.cpp
    SpriteRenderer.cpp
    GLLoader.cpp
)

add_executable(gpu_demo WIN32 ${SOURCES})

# Use wxWidgets targets for automatic include directory management
target_link_libraries(gpu_demo PRIVATE
    wx::core
    wx::base
    wx::gl
    OpenGL::GL
    landstalker
)

target_include_directories(gpu_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/liblandstalker/landstalker/include
)

if(${LANDSTALKER_BUILD_SHARED})
    target_link_libraries(gpu_demo PRIVATE png_shared ZLIB::ZLIB)
else()
    target_link_libraries(gpu_demo PRIVATE png_static ZLIB::ZLIB)
endif()

# Copy shaders to build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/shaders DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
