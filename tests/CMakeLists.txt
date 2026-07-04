add_executable(kumo_tests
    test_math.cpp
)
target_link_libraries(kumo_tests PRIVATE doctest::doctest kumo_core kumo_math)
kumo_target_warnings(kumo_tests)
if(MSVC)
    # doctest 2.4.12 specializes std::tuple, which recent MSVC flags as C5285.
    target_compile_options(kumo_tests PRIVATE /wd5285)
endif()
add_test(NAME kumo_tests COMMAND kumo_tests)
