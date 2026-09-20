cmake_minimum_required(VERSION 3.20)
project(usdprep VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenUSD (pass -Dpxr_DIR=<usd-install> at configure time).
# OpenGL must be found first: pxr's garch target links OpenGL::GL.
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)

option(USDPREP_BUILD_CLI "Build the usdcut command line tool" ON)
option(USDPREP_BUILD_TESTS "Build tests" ON)

add_subdirectory(src/core)
if(USDPREP_BUILD_CLI)
    add_subdirectory(src/cli)
endif()
if(USDPREP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
