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

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 4be08b1ecf7709f15e4274fb2ddac37e121d7d9a # docking branch
)

FetchContent_MakeAvailable(glm doctest glfw imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
)
target_include_directories(imgui_lib SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)

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

    # imgui_impl_metal.mm uses manual retain/release; do not enable ARC for it.
    target_sources(imgui_lib PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
    target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_METAL_CPP)
    target_link_libraries(imgui_lib PUBLIC metal_cpp)
endif()
