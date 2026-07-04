add_executable(kumo_tests
    test_math.cpp
)
target_link_libraries(kumo_tests PRIVATE doctest::doctest kumo_core kumo_math)
kumo_target_warnings(kumo_tests)
add_test(NAME kumo_tests COMMAND kumo_tests)
