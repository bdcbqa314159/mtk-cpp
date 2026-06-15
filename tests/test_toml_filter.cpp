#include <doctest/doctest.h>

#include <string>

#include "core/toml_filter.hpp"

TEST_CASE("toml_filter parse_all reads a basic filter") {
    std::string src = R"(
[filters.make]
description = "Make output"
match_command = "^make$"
strip_ansi = true
strip_lines_matching = ["^Entering directory"]
keep_lines_matching = ["error", "warning"]
truncate_lines_at = 80
max_lines = 20
on_empty = "[mtk: clean build]"
)";
    auto filters = mtk::core::toml_filter::parse_all(src);
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];
    CHECK(f.name == "make");
    CHECK(f.match_command_pattern == "^make$");
    CHECK(f.strip_ansi);
    CHECK(f.strip_lines_matching.size() == 1);
    CHECK(f.keep_lines_matching.size() == 2);
    CHECK(f.truncate_lines_at.has_value());
    CHECK(*f.truncate_lines_at == 80);
    CHECK(*f.max_lines == 20);
    CHECK(*f.on_empty == "[mtk: clean build]");
}

TEST_CASE("toml_filter command_matches uses regex") {
    mtk::core::toml_filter::Filter f;
    f.match_command_pattern = "^make$";
    mtk::core::toml_filter::compile(f);
    CHECK(mtk::core::toml_filter::command_matches(f, "make"));
    CHECK_FALSE(mtk::core::toml_filter::command_matches(f, "cmake"));
}

TEST_CASE("toml_filter apply runs the pipeline") {
    mtk::core::toml_filter::Filter f;
    f.strip_lines_matching = {"^DEBUG"};
    f.keep_lines_matching = {"error", "warning"};
    f.max_lines = 5;
    mtk::core::toml_filter::compile(f);

    std::string input =
        "DEBUG: starting\n"
        "info: doing things\n"
        "warning: something off\n"
        "error: boom\n"
        "info: trailing\n"
        "DEBUG: closing\n";

    auto out = mtk::core::toml_filter::apply(f, input);
    CHECK(out.find("DEBUG") == std::string::npos);
    CHECK(out.find("warning") != std::string::npos);
    CHECK(out.find("error") != std::string::npos);
    CHECK(out.find("info") == std::string::npos);
}

TEST_CASE("toml_filter apply uses on_empty when result is empty") {
    mtk::core::toml_filter::Filter f;
    f.keep_lines_matching = {"NEVERMATCHES_XYZ"};
    f.on_empty = "[mtk: nothing of note]";
    mtk::core::toml_filter::compile(f);
    auto out = mtk::core::toml_filter::apply(f, "line a\nline b\n");
    CHECK(out == "[mtk: nothing of note]");
}

TEST_CASE("toml_filter apply drops middle when max_lines exceeded") {
    mtk::core::toml_filter::Filter f;
    f.max_lines = 4;
    std::string input;
    for (int i = 0; i < 20; ++i) input += "line " + std::to_string(i) + "\n";
    auto out = mtk::core::toml_filter::apply(f, input);
    CHECK(out.find("lines dropped") != std::string::npos);
    CHECK(out.find("line 0") != std::string::npos);
    CHECK(out.find("line 19") != std::string::npos);
}
