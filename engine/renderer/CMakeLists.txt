add_library(kumo_renderer STATIC
    src/forward_renderer.cpp
    src/ibl.cpp
    src/shader_load.cpp
)
target_include_directories(kumo_renderer PUBLIC include)
# shaderc is PUBLIC: forward_renderer.h's setMaterialShader (ADR 0011) returns
# shaderc::CompileError, so consumers of the header need its include path too.
target_link_libraries(kumo_renderer
    PUBLIC kumo_rhi kumo_scene kumo_asset kumo_core kumo_math kumo_shaderc
)
# Development builds compile shaders from the source tree (see docs/shaders.md);
# relocatable packaging is deferred to M7.
target_compile_definitions(kumo_renderer PRIVATE KUMO_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
kumo_target_warnings(kumo_renderer)
