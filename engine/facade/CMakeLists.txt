add_library(kumo_facade STATIC src/engine_runtime.cpp src/undo_stack.cpp src/orbit_camera.cpp
                               src/scene_spec.cpp src/director.cpp)
target_include_directories(kumo_facade PUBLIC include)
# engine_runtime.cpp is the composition root (ADR 0037) and constructs the
# concrete PolyHavenClient wired into asset_fetch's SceneToolContext callback;
# asset_fetch.h is not a public kumo_agent header (private like kumo_agent's
# own base64.h), reached into the same way tests reach into kumo_agent's
# retry_after.h.
target_include_directories(kumo_facade PRIVATE ${CMAKE_SOURCE_DIR}/engine/agent/src)
# Facade is the top of the engine stack (ADR 0043/0044): its public headers
# stay free of GLFW/ImGui/platform types so the SwiftUI shell can consume it
# alongside the GLFW dev viewer.
target_link_libraries(kumo_facade
    PUBLIC kumo_gpu kumo_agent kumo_renderer kumo_scene kumo_asset kumo_core kumo_math
    PRIVATE nlohmann_json::nlohmann_json)
kumo_target_warnings(kumo_facade)
