set(VIEWER_SOURCES main.cpp)
if(APPLE)
    list(APPEND VIEWER_SOURCES app.cpp ui.cpp metal_layer_glue.mm)
endif()

add_executable(viewer ${VIEWER_SOURCES})
target_link_libraries(viewer PRIVATE kumo_core kumo_math glfw)
target_compile_definitions(viewer PRIVATE KUMO_VERSION_STRING="${PROJECT_VERSION}")
target_compile_definitions(viewer PRIVATE KUMO_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
target_compile_definitions(viewer PRIVATE KUMO_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
kumo_target_warnings(viewer)

if(APPLE)
    set_source_files_properties(metal_layer_glue.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # kumo_facade transitively provides kumo_rhi/kumo_shaderc/kumo_scene/kumo_asset/
    # kumo_renderer/kumo_agent (all PUBLIC in its own link); kumo_rhi_metal is the
    # concrete Device the shell creates, which facade itself stays agnostic of.
    target_link_libraries(viewer PRIVATE kumo_facade kumo_rhi_metal imgui_lib "-framework Cocoa")
endif()
