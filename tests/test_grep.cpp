#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cmds/grep.hpp"
#include "core/utils.hpp"

namespace {
std::string read_fixture(const std::string& name) {
    auto path = std::filesystem::path(MTK_FIXTURES_DIR) / name;
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "fixture missing: " << path.string());
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}
}  // namespace

TEST_CASE("parse_match_line accepts NUL-separated rg/grep output") {
    using mtk::cmds::grep::internal::parse_match_line;
    std::string line = "file.cpp";
    line.push_back('\0');
    line += "42:int main() {}";
    auto p = parse_match_line(line);
    REQUIRE(p.has_value());
    CHECK(p->file == "file.cpp");
    CHECK(p->line_num == 42);
    CHECK(p->content == "int main() {}");
}

TEST_CASE("parse_match_line handles content with embedded :digits: tokens") {
    using mtk::cmds::grep::internal::parse_match_line;
    std::string line = "log.txt";
    line.push_back('\0');
    line += "7:debug: counter is :42: now";
    auto p = parse_match_line(line);
    REQUIRE(p.has_value());
    CHECK(p->content == "debug: counter is :42: now");
}

TEST_CASE("parse_match_line falls back to colon-separated format") {
    using mtk::cmds::grep::internal::parse_match_line;
    auto p = parse_match_line("src/file.cpp:42:int main() {}");
    REQUIRE(p.has_value());
    CHECK(p->file == "src/file.cpp");
    CHECK(p->line_num == 42);
    CHECK(p->content == "int main() {}");
}

TEST_CASE("parse_match_line rejects truly malformed input") {
    using mtk::cmds::grep::internal::parse_match_line;
    CHECK_FALSE(parse_match_line("").has_value());
    CHECK_FALSE(parse_match_line("no colons at all").has_value());
    CHECK_FALSE(parse_match_line("file:not_a_number:content").has_value());
    CHECK_FALSE(parse_match_line(":42:content").has_value());
}

TEST_CASE("has_format_flag detects passthrough-triggering flags") {
    using mtk::cmds::grep::internal::has_format_flag;
    CHECK(has_format_flag({"-c"}));
    CHECK(has_format_flag({"--count"}));
    CHECK(has_format_flag({"-l"}));
    CHECK(has_format_flag({"--files-with-matches"}));
    CHECK(has_format_flag({"-o"}));
    CHECK_FALSE(has_format_flag({"-i"}));
    CHECK_FALSE(has_format_flag({"-A", "3"}));
}

TEST_CASE("clean_line trims, truncates, and centers on pattern when too long") {
    using mtk::cmds::grep::internal::clean_line;
    CHECK(clean_line("    short hit    ", 50, "hit") == "short hit");
    auto long_line =
        std::string(50, 'x') + " needle " + std::string(50, 'y');
    auto cleaned = clean_line(long_line, 30, "needle");
    CHECK(cleaned.find("needle") != std::string::npos);
    CHECK(cleaned.size() <= 36);
}

TEST_CASE("clean_line case-insensitive pattern match in long line") {
    using mtk::cmds::grep::internal::clean_line;
    auto raw = std::string(60, '.') + "WHAT_I_WANT" + std::string(60, '.');
    auto out = mtk::cmds::grep::internal::clean_line(raw, 40, "what_i_want");
    CHECK(out.find("WHAT_I_WANT") != std::string::npos);
}

TEST_CASE("compact_path shortens long paths and keeps short ones intact") {
    using mtk::cmds::grep::internal::compact_path;
    CHECK(compact_path("src/file.cpp") == "src/file.cpp");
    auto long_path =
        "/very/long/path/with/many/segments/that/goes/on/and/on/until/it/is/large/file.cpp";
    auto out = compact_path(long_path);
    CHECK(out.find("...") != std::string::npos);
    CHECK(out.find("file.cpp") != std::string::npos);
}

TEST_CASE("translate_bre_alternation converts BRE to PCRE alternation") {
    using mtk::cmds::grep::internal::translate_bre_alternation;
    CHECK(translate_bre_alternation(R"(fn foo\|pub bar)") == "fn foo|pub bar");
    CHECK(translate_bre_alternation("plain") == "plain");
}

TEST_CASE("parses every line of the rg fixture without error") {
    auto raw = read_fixture("grep_rg_raw.txt");
    std::size_t parsed = 0;
    for (const auto& line : mtk::core::utils::split_lines(raw)) {
        if (line.empty()) continue;
        auto p = mtk::cmds::grep::internal::parse_match_line(line);
        if (p) ++parsed;
    }
    CHECK(parsed >= 10);
}
