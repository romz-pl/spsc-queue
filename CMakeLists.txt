cmake_minimum_required( VERSION 3.21 )

project( spsc-queue CXX )

set( CMAKE_CXX_STANDARD 23 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )
set( CMAKE_BUILD_TYPE Release )

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic -march=native -mtune=native")

# Configure a project for testing with CTest/CDash
# See https://cmake.org/cmake/help/latest/module/CTest.html
include(CTest)


# Defines functions to help use the Google Test infrastructure
# See https://cmake.org/cmake/help/latest/module/FindGTest.html
find_package(GTest REQUIRED)
include(GoogleTest)

add_subdirectory(src)
add_subdirectory(tests)

