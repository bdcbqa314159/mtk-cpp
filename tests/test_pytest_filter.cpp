#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/toml_filter.hpp"

namespace {

// Load the shipped filters/pytest.toml (relative to the fixtures dir) so
// the test exercises the real artifact, not a drifting copy.
std::string load_pytest_filter() {
    std::ifstream in(std::string(MTK_FIXTURES_DIR) + "/../../filters/pytest.toml");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A realistic verbose-mode pytest run: 149 passes, 1 failure, the FAILURES
// block, and pytest's own summary line.
std::string sample_pytest_output() {
    std::string s;
    s += "============================= test session starts ==============================\n";
    s += "platform darwin -- Python 3.12.0, pytest-8.0.0, pluggy-1.4.0\n";
    s += "rootdir: /Users/dev/project\n";
    s += "plugins: cov-4.1.0, mock-3.12.0\n";
    s += "collected 150 items\n\n";
    for (int i = 0; i < 148; ++i) {
        char line[160];
        std::snprintf(line, sizeof(line),
                      "tests/test_mod.py::test_case_%03d PASSED                          [%3d%%]\n",
                      i, (i * 100) / 150);
        s += line;
    }
    s += "tests/test_mod.py::test_broken FAILED                            [ 99%]\n";
    s += "tests/test_mod.py::test_last PASSED                              [100%]\n\n";
    s += "=================================== FAILURES ===================================\n";
    s += "_________________________________ test_broken _________________________________\n\n";
    s += "    def test_broken():\n";
    s += ">       assert add(2, 2) == 5\n";
    s += "E       assert 4 == 5\n\n";
    s += "tests/test_mod.py:42: AssertionError\n";
    s += "=========================== short test summary info ============================\n";
    s += "FAILED tests/test_mod.py::test_broken - assert 4 == 5\n";
    s += "======================== 1 failed, 149 passed in 2.34s =========================\n";
    return s;
}

}  // namespace

TEST_CASE("pytest filter: keeps the signal, drops the passes, and measurably shrinks") {
    auto filters = mtk::core::toml_filter::parse_all(load_pytest_filter());
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];
    CHECK(mtk::core::toml_filter::command_matches(f, "pytest"));

    const std::string input = sample_pytest_output();
    const std::string out = mtk::core::toml_filter::apply(f, input);

    // Signal preserved: the failure, its assertion, the file:line, and the
    // summary pytest printed itself.
    CHECK(out.find("test_broken FAILED") != std::string::npos);
    CHECK(out.find("assert 4 == 5") != std::string::npos);
    CHECK(out.find("tests/test_mod.py:42") != std::string::npos);
    CHECK(out.find("1 failed, 149 passed") != std::string::npos);

    // Noise gone: no per-test PASSED line survives.
    CHECK(out.find("PASSED") == std::string::npos);

    // Measurement (spike): print the before/after so we can judge whether a
    // dedicated count_summary primitive is even worth building.
    const long pct = 100 - static_cast<long>(out.size() * 100 / input.size());
    std::printf("[pytest spike] bytes_in=%zu bytes_out=%zu savings=%ld%%\n",
                input.size(), out.size(), pct);

    CHECK(out.size() < input.size());
}
