add_library(kumo_agent STATIC
    src/confirmation_gate.cpp
    src/fake_provider.cpp
    src/scene_tools.cpp
    src/session.cpp
    src/tool_registry.cpp
)
target_include_directories(kumo_agent PUBLIC include)
# Public headers carry JSON as std::string, so nlohmann and the renderer stay
# private implementation details.
target_link_libraries(kumo_agent
    PUBLIC kumo_core kumo_math kumo_scene
    PRIVATE kumo_asset kumo_renderer nlohmann_json::nlohmann_json)
kumo_target_warnings(kumo_agent)
