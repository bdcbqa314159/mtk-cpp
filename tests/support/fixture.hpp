#pragma once

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Per CR7: read_fixture lives in ONE place. Tests #include this header
// rather than redefining a local copy. MTK_FIXTURES_DIR is set on the
// mtk_tests target (tests/CMakeLists.txt).
inline std::string read_fixture(const std::string& name) {
    auto path = std::filesystem::path(MTK_FIXTURES_DIR) / name;
    std::ifstream f(path, std::ios::binary);  // grep fixtures embed NUL bytes
    REQUIRE_MESSAGE(f.good(), "fixture missing: " << path.string());
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}
