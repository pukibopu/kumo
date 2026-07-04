include(FetchContent)

# All third-party dependencies are declared here, pinned to exact commits.

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863 # 1.0.1
)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG 1da23a3e8119ec5cce4f9388e91b065e20bf06f5 # v2.4.12
)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 7b6aead9fb88b3623e3b3725ebb42670cbe4c579 # 3.4
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glm doctest glfw)

if(APPLE)
    # Mirror of Apple's metal-cpp distribution (developer.apple.com/metal/cpp).
    FetchContent_Declare(metalcpp
        GIT_REPOSITORY https://github.com/bkaradzic/metal-cpp.git
        GIT_TAG c9727bc9468a90d7ea8fc89d5ee03b8d8992a570
    )
    FetchContent_MakeAvailable(metalcpp)
    add_library(metal_cpp INTERFACE)
    target_include_directories(metal_cpp SYSTEM INTERFACE ${metalcpp_SOURCE_DIR})
    target_link_libraries(metal_cpp INTERFACE "-framework Foundation" "-framework Metal"
                                              "-framework QuartzCore")
endif()
