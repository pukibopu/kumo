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

# Header-only asset loaders. SOURCE_SUBDIR points at a path with no CMakeLists so
# MakeAvailable only populates the sources; we wrap them as INTERFACE targets below.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG 360db1a95480fe102ae9c69b27c5d101167ff5ba # v1.15
    SOURCE_SUBDIR do-not-add
)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4 # master 2026-07
    SOURCE_SUBDIR do-not-add
)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 55f93686c01528224f448c19128836e7df245f72 # v3.12.0
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
# Upstream knob that marks the interface include dir SYSTEM, so -Wall/-Werror stay
# clean without depending on the CMake 3.25 SYSTEM target property.
set(JSON_SystemInclude ON CACHE BOOL "" FORCE)

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

FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 8a85691a0740d390761a1008b4696f57facd02c4 # 15.4.0
)

FetchContent_Declare(spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG 1a6169566c73d3da552748fc372fe2bbb856e46e # vulkan-sdk-1.4.350.1
)

FetchContent_MakeAvailable(glm doctest nlohmann_json glfw imgui)

FetchContent_MakeAvailable(cgltf stb)
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

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

# Shader cross-compilation toolchain (kumo_shaderc): GLSL -> SPIR-V -> MSL.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glslang spirv_cross)

# Treat third-party headers as system headers so -Wall/-Werror do not fire on them.
foreach(_kumo_thirdparty
        glslang glslang-default-resource-limits SPIRV MachineIndependent GenericCodeGen
        OSDependent SPIRV-Tools SPIRV-Tools-opt
        spirv-cross-core spirv-cross-glsl spirv-cross-msl)
    if(TARGET ${_kumo_thirdparty})
        set_target_properties(${_kumo_thirdparty} PROPERTIES SYSTEM ON)
    endif()
endforeach()
