add_library(kumo_agent STATIC
    src/claude_codec.cpp
    src/config.cpp
    src/confirmation_gate.cpp
    src/fake_provider.cpp
    src/http_provider.cpp
    src/openai_codec.cpp
    src/scene_tools.cpp
    src/session.cpp
    src/summary.cpp
    src/tool_registry.cpp
)
if(APPLE)
    target_sources(kumo_agent PRIVATE src/http_transport_apple.mm)
    set_source_files_properties(src/http_transport_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(kumo_agent PRIVATE "-framework Foundation")
endif()
target_include_directories(kumo_agent PUBLIC include)
# Public headers carry JSON as std::string, so nlohmann and the renderer stay
# private implementation details.
target_link_libraries(kumo_agent
    PUBLIC kumo_core kumo_math kumo_scene
    PRIVATE kumo_asset kumo_renderer nlohmann_json::nlohmann_json)
kumo_target_warnings(kumo_agent)
