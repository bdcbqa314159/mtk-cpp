add_executable(mtk_tests
    test_main.cpp
    test_git_log.cpp
    test_toml_filter.cpp
    test_utils.cpp
)

target_link_libraries(mtk_tests PRIVATE mtk_lib doctest::doctest)
target_compile_definitions(mtk_tests PRIVATE
    MTK_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME mtk_tests COMMAND mtk_tests)
