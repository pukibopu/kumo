add_executable(kumo_tests
    test_core_file.cpp
    test_main_thread_queue.cpp
    test_math.cpp
    test_math_m4.cpp
    test_scene.cpp
    test_asset.cpp
    test_primitives.cpp
    test_shaderc.cpp
    test_shader_snapshots.cpp
    test_agent_tool_registry.cpp
    test_agent_scene_tools.cpp
    test_agent_session.cpp
    test_agent_openai_codec.cpp
    test_agent_claude_codec.cpp
    test_agent_http_provider.cpp
    test_agent_config.cpp
)
target_link_libraries(kumo_tests PRIVATE doctest::doctest kumo_core kumo_math kumo_shaderc
    kumo_scene kumo_asset kumo_agent nlohmann_json::nlohmann_json)
target_compile_definitions(kumo_tests PRIVATE
    KUMO_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
    KUMO_SNAPSHOT_DIR="${CMAKE_SOURCE_DIR}/tests/snapshots"
    KUMO_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
    KUMO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
kumo_target_warnings(kumo_tests)
if(MSVC)
    # doctest 2.4.12 specializes std::tuple, which recent MSVC flags as C5285.
    target_compile_options(kumo_tests PRIVATE /wd5285)
    # std::getenv trips C4996 under /WX; it is standard and portable.
    target_compile_definitions(kumo_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(APPLE)
    target_sources(kumo_tests PRIVATE test_metal_binding.cpp)
    target_include_directories(kumo_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/rhi_metal/include)
endif()

add_test(NAME kumo_tests COMMAND kumo_tests)

# Golden image tests render on the GPU against per-machine baselines; they are
# excluded in CI via `ctest -LE golden` and run locally with `ctest -L golden`.
if(APPLE)
    add_executable(kumo_golden test_golden.cpp)
    target_link_libraries(kumo_golden PRIVATE doctest::doctest kumo_core kumo_math kumo_rhi
        kumo_scene kumo_asset kumo_renderer kumo_rhi_metal)
    target_compile_definitions(kumo_golden PRIVATE
        KUMO_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
        KUMO_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden"
        KUMO_GOLDEN_ACTUAL_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    kumo_target_warnings(kumo_golden)
    add_test(NAME kumo_golden COMMAND kumo_golden)
    set_tests_properties(kumo_golden PROPERTIES LABELS golden)
endif()
