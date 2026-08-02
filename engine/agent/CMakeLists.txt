add_library(kumo_agent STATIC
    src/asset_fetch.cpp
    src/asset_index.cpp
    src/placement.cpp
    src/base64.cpp
    src/claude_codec.cpp
    src/config.cpp
    src/confirmation_gate.cpp
    src/fake_provider.cpp
    src/http_provider.cpp
    src/mcp_server.cpp
    src/openai_codec.cpp
    src/retry_after.cpp
    src/scene_tools.cpp
    src/session.cpp
    src/shader_tools.cpp
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
# private implementation details. shaderc is PUBLIC: shader_tools.h's
# ShaderToolContext exposes shaderc::CompileError, so consumers of the header
# need its include path too (same reasoning as kumo_renderer's own PUBLIC dep).
target_link_libraries(kumo_agent
    PUBLIC kumo_core kumo_math kumo_scene kumo_shaderc
    PRIVATE kumo_asset kumo_renderer nlohmann_json::nlohmann_json)
kumo_target_warnings(kumo_agent)
