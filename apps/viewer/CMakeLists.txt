add_executable(viewer main.cpp)
target_link_libraries(viewer PRIVATE kumo_core kumo_math glfw)
target_compile_definitions(viewer PRIVATE KUMO_VERSION_STRING="${PROJECT_VERSION}")
kumo_target_warnings(viewer)
