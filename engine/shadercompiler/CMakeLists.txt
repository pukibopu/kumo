add_library(kumo_shaderc STATIC
    src/compiler.cpp
    src/msl_translate.cpp
)
target_include_directories(kumo_shaderc PUBLIC include)
target_link_libraries(kumo_shaderc
    PUBLIC kumo_core
    PRIVATE glslang glslang-default-resource-limits SPIRV spirv-cross-msl
)
kumo_target_warnings(kumo_shaderc)
