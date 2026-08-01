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
    # kumo_facade transitively provides the engine stack; the shell links the
    # concrete Metal GPU facade to create its device and surface.
    target_link_libraries(viewer PRIVATE kumo_facade kumo_gpu imgui_lib "-framework Cocoa")
endif()
