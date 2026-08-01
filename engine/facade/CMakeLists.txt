add_library(kumo_facade STATIC src/engine_runtime.cpp src/undo_stack.cpp src/orbit_camera.cpp)
target_include_directories(kumo_facade PUBLIC include)
# Facade is the top of the engine stack (ADR 0043/0044): its public headers
# stay free of GLFW/ImGui/platform types so the SwiftUI shell can consume it
# alongside the GLFW dev viewer.
target_link_libraries(kumo_facade
    PUBLIC kumo_gpu kumo_agent kumo_renderer kumo_scene kumo_asset kumo_core kumo_math
    PRIVATE nlohmann_json::nlohmann_json)
kumo_target_warnings(kumo_facade)
