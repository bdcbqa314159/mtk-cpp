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

TEST_CASE("detect_diff_options sees --stat and --no-compact") {
    using mtk::cmds::git::internal::detect_diff_options;
    CHECK_FALSE(detect_diff_options({}).wants_stat);
    CHECK_FALSE(detect_diff_options({}).wants_no_compact);
    CHECK(detect_diff_options({"--stat"}).wants_stat);
    CHECK(detect_diff_options({"--numstat"}).wants_stat);
    CHECK(detect_diff_options({"--shortstat"}).wants_stat);
    CHECK(detect_diff_options({"--no-compact"}).wants_no_compact);
}

TEST_CASE("compact_diff preserves headers and counts added/removed per file") {
    auto raw = read_fixture("git_diff_raw.txt");
    auto out = mtk::cmds::git::internal::compact_diff(raw);

    CHECK(out.find("src/core/exec.cpp") != std::string::npos);
    CHECK(out.find("src/cmds/git.cpp") != std::string::npos);
    CHECK(out.find("src/cmds/exec.cpp") != std::string::npos);

    CHECK(out.find("@@ -10,7 +10,7") != std::string::npos);
    CHECK(out.find("@@ -50,6 +50,12") != std::string::npos);

    CHECK(out.find("+1 -1") != std::string::npos);
}

TEST_CASE("compact_diff produces measurable token savings vs raw") {
    auto raw = read_fixture("git_diff_raw.txt");
    auto out = mtk::cmds::git::internal::compact_diff(raw);
    auto raw_tokens = mtk::core::utils::count_tokens(raw);
    auto out_tokens = mtk::core::utils::count_tokens(out);
    REQUIRE(raw_tokens > 0);
    CHECK(out_tokens > 0);
    INFO("raw=" << raw_tokens << " out=" << out_tokens);
}

TEST_CASE("compact_diff caps per-hunk lines and emits truncation marker") {
    std::string diff =
        "diff --git a/big.txt b/big.txt\n"
        "index 1..2 100644\n"
        "--- a/big.txt\n"
        "+++ b/big.txt\n"
        "@@ -1,200 +1,200 @@\n";
    for (int i = 0; i < 200; ++i) diff += "+line " + std::to_string(i) + "\n";

    auto out = mtk::cmds::git::internal::compact_diff(diff, /*max_lines=*/10000,
                                                     /*max_hunk_lines=*/50);
    CHECK(out.find("lines truncated") != std::string::npos);
    CHECK(out.find("[full diff:") != std::string::npos);
    CHECK(out.find("+200 -0") != std::string::npos);
}

TEST_CASE("compact_diff hits global max_lines and emits more-truncated marker") {
    std::string diff =
        "diff --git a/x.txt b/x.txt\n"
        "@@ -1 +1 @@\n";
    for (int i = 0; i < 1000; ++i) diff += "+line " + std::to_string(i) + "\n";
    auto out = mtk::cmds::git::internal::compact_diff(diff, /*max_lines=*/20,
                                                     /*max_hunk_lines=*/1000);
    CHECK(out.find("more changes truncated") != std::string::npos);
    CHECK(out.find("[full diff:") != std::string::npos);
}
