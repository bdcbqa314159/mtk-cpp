#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cmds/git.hpp"
#include "core/utils.hpp"

namespace {
std::string read_fixture(const std::string& name) {
    auto path = std::filesystem::path(MTK_FIXTURES_DIR) / name;
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.good(), "fixture missing: " << path.string());
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}
}  // namespace

TEST_CASE("git log filter compresses commits") {
    auto raw = read_fixture("git_log_raw.txt");
    auto filtered = mtk::cmds::git::internal::filter_log_output(raw, 10, 80, 3);

    CHECK(!filtered.empty());
    CHECK(filtered.find("abc1234") != std::string::npos);
    CHECK(filtered.find("def5678") != std::string::npos);

    auto raw_tokens = mtk::core::utils::count_tokens(raw);
    auto out_tokens = mtk::core::utils::count_tokens(filtered);
    REQUIRE(raw_tokens > 0);
    double savings = 100.0 - (static_cast<double>(out_tokens) / raw_tokens * 100.0);
    INFO("savings = " << savings << "%");
    CHECK(savings >= 30.0);
}

TEST_CASE("git log filter caps body lines and emits omitted-count marker") {
    auto raw = read_fixture("git_log_raw.txt");
    auto filtered = mtk::cmds::git::internal::filter_log_output(raw, 10, 80, 3);
    CHECK(filtered.find("[+") != std::string::npos);
    CHECK(filtered.find("body lines omitted]") != std::string::npos);
}

TEST_CASE("git log filter strips Signed-off-by and Co-authored-by") {
    auto raw = read_fixture("git_log_raw.txt");
    auto filtered = mtk::cmds::git::internal::filter_log_output(raw, 10, 80, 3);
    CHECK(filtered.find("Signed-off-by:") == std::string::npos);
    CHECK(filtered.find("Co-authored-by:") == std::string::npos);
}

TEST_CASE("detect_log_options recognises user-set count and format") {
    using mtk::cmds::git::internal::detect_log_options;

    auto a = detect_log_options({});
    CHECK_FALSE(a.user_set_count);
    CHECK_FALSE(a.user_set_format);

    auto b = detect_log_options({"-5"});
    CHECK(b.user_set_count);

    auto c = detect_log_options({"-n", "20"});
    CHECK(c.user_set_count);

    auto d = detect_log_options({"--max-count=50"});
    CHECK(d.user_set_count);

    auto e = detect_log_options({"--pretty=oneline"});
    CHECK(e.user_set_format);
    CHECK_FALSE(e.user_set_count);

    auto f = detect_log_options({"--format=%H"});
    CHECK(f.user_set_format);
}
