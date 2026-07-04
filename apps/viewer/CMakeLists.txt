set(VIEWER_SOURCES main.cpp)
if(APPLE)
    list(APPEND VIEWER_SOURCES app_metal.cpp metal_layer_glue.mm)
endif()

add_executable(viewer ${VIEWER_SOURCES})
target_link_libraries(viewer PRIVATE kumo_core kumo_math glfw)
target_compile_definitions(viewer PRIVATE KUMO_VERSION_STRING="${PROJECT_VERSION}")
kumo_target_warnings(viewer)

if(APPLE)
    set_source_files_properties(metal_layer_glue.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(viewer PRIVATE metal_cpp "-framework Cocoa")
endif()
