#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/toml_filter.hpp"

namespace {

std::string load_npm_filter() {
    std::ifstream in(std::string(MTK_FIXTURES_DIR) + "/../../filters/npm.toml");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A realistic `npm install`: deprecation spam, the install summary, the
// funding nag, and a vulnerability tally.
std::string sample_npm_install() {
    std::string s;
    s += "npm warn deprecated inflight@1.0.6: This module is not supported, and leaks memory.\n";
    s += "npm warn deprecated glob@7.2.3: Glob versions prior to v9 are no longer supported\n";
    s += "npm warn deprecated rimraf@3.0.2: Rimraf versions prior to v4 are no longer supported\n";
    s += "\n";
    s += "added 247 packages, and audited 248 packages in 12s\n";
    s += "\n";
    s += "34 packages are looking for funding\n";
    s += "  run `npm fund` for details\n";
    s += "\n";
    s += "found 3 vulnerabilities (1 low, 2 moderate)\n";
    s += "  run `npm audit fix` to address them, or `npm audit` for details\n";
    return s;
}

}  // namespace

TEST_CASE("npm filter: drops ceremony, keeps install summary + vulnerabilities") {
    auto filters = mtk::core::toml_filter::parse_all(load_npm_filter());
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];
    CHECK(mtk::core::toml_filter::command_matches(f, "npm"));

    // Regression guard: npm writes warnings/notices to stderr, so the filter
    // MUST merge stderr before stripping — otherwise the chatter is invisible.
    CHECK(f.filter_stderr);

    const std::string input = sample_npm_install();
    const std::string out = mtk::core::toml_filter::apply(f, input);

    // Signal preserved: the install summary and the vulnerability tally.
    CHECK(out.find("added 247 packages") != std::string::npos);
    CHECK(out.find("found 3 vulnerabilities") != std::string::npos);

    // Ceremony gone: deprecation spam and the funding nag.
    CHECK(out.find("npm warn deprecated") == std::string::npos);
    CHECK(out.find("looking for funding") == std::string::npos);
    CHECK(out.find("npm fund") == std::string::npos);

    const long pct = 100 - static_cast<long>(out.size() * 100 / input.size());
    std::printf("[npm spike] bytes_in=%zu bytes_out=%zu savings=%ld%%\n",
                input.size(), out.size(), pct);

    CHECK(out.size() < input.size());
}

TEST_CASE("npm filter: errors survive (correctness harness)") {
    auto filters = mtk::core::toml_filter::parse_all(load_npm_filter());
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];

    const std::string input =
        "npm error code ELIFECYCLE\n"
        "npm error path /app\n"
        "npm error command failed\n"
        "npm notice\n"
        "npm notice New major version of npm available! 10.2.0 -> 11.0.0\n";
    const std::string out = mtk::core::toml_filter::apply(f, input);

    CHECK(out.find("npm error code ELIFECYCLE") != std::string::npos);
    CHECK(out.find("command failed") != std::string::npos);
    // The "new version" notice is stripped, the errors are not.
    CHECK(out.find("New major version") == std::string::npos);
}
