cmake_minimum_required(VERSION 3.24)

project(kumo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KUMO_BUILD_TESTS "Build unit tests" ON)
option(KUMO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(dependencies)

add_subdirectory(engine)
add_subdirectory(apps)

if(KUMO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
