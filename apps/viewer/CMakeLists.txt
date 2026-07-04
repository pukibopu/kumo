set(VIEWER_SOURCES main.cpp)
if(APPLE)
    list(APPEND VIEWER_SOURCES app_metal.mm)
endif()

add_executable(viewer ${VIEWER_SOURCES})
target_link_libraries(viewer PRIVATE kumo_core kumo_math glfw)
target_compile_definitions(viewer PRIVATE KUMO_VERSION_STRING="${PROJECT_VERSION}")
kumo_target_warnings(viewer)

if(APPLE)
    set_source_files_properties(app_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(viewer PRIVATE "-framework Metal" "-framework QuartzCore"
                                         "-framework Cocoa")
endif()
