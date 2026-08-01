cmake_minimum_required(VERSION 3.24)

project(kumo VERSION 0.1.0 LANGUAGES C CXX)

if(NOT APPLE)
    message(FATAL_ERROR "kumo supports Apple platforms only (macOS and iOS)")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(KUMO_DESKTOP_DEFAULT OFF)
    set(KUMO_TESTS_DEFAULT OFF)
else()
    set(KUMO_DESKTOP_DEFAULT ON)
    set(KUMO_TESTS_DEFAULT ON)
endif()

option(KUMO_BUILD_DESKTOP_APPS "Build the macOS viewer and SwiftUI app"
       ${KUMO_DESKTOP_DEFAULT})
option(KUMO_BUILD_TESTS "Build unit tests" ${KUMO_TESTS_DEFAULT})
option(KUMO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(dependencies)

add_subdirectory(engine)
if(KUMO_BUILD_DESKTOP_APPS)
    add_subdirectory(apps)
endif()

if(KUMO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
