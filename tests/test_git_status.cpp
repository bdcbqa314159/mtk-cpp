#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cmds/git.hpp"

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

TEST_CASE("uses_compact_status_path: empty + branch/short flag combinations") {
    using mtk::cmds::git::internal::uses_compact_status_path;
    CHECK(uses_compact_status_path({}));
    CHECK(uses_compact_status_path({"-b"}));
    CHECK(uses_compact_status_path({"--branch"}));
    CHECK(uses_compact_status_path({"-sb"}));
    CHECK(uses_compact_status_path({"-bs"}));
    CHECK_FALSE(uses_compact_status_path({"-s"}));
    CHECK(uses_compact_status_path({"-s", "-b"}));
    CHECK_FALSE(uses_compact_status_path({"--porcelain=v2"}));
    CHECK_FALSE(uses_compact_status_path({"path/to/file"}));
}

TEST_CASE("format_status_output collapses branch line and lists changes") {
    auto raw = read_fixture("git_status_porcelain.txt");
    auto out = mtk::cmds::git::internal::format_status_output(raw);
    CHECK(out.find("* main") != std::string::npos);
    CHECK(out.find("origin/main") != std::string::npos);
    CHECK(out.find("M src/core/exec.cpp") != std::string::npos);
    CHECK(out.find("?? .mtk/") != std::string::npos);
}

TEST_CASE("format_status_output emits clean marker when no changes") {
    auto out = mtk::cmds::git::internal::format_status_output("## main\n");
    CHECK(out.find("* main") != std::string::npos);
    CHECK(out.find("clean — nothing to commit") != std::string::npos);
}

TEST_CASE("format_status_output uses detached ref when supplied") {
    auto out = mtk::cmds::git::internal::format_status_output(
        "## HEAD (no branch)\n", "HEAD detached at abc1234");
    CHECK(out.find("HEAD detached at abc1234") != std::string::npos);
    CHECK(out.find("(no branch)") == std::string::npos);
}

TEST_CASE("extract_state_header recognises interactive rebase") {
    auto raw = read_fixture("git_status_plain_rebase.txt");
    auto state = mtk::cmds::git::internal::extract_state_header(raw);
    REQUIRE(state.has_value());
    CHECK(*state == "rebase in progress");
}

TEST_CASE("extract_state_header returns nullopt for clean status") {
    auto state = mtk::cmds::git::internal::extract_state_header(
        "On branch main\nnothing to commit, working tree clean\n");
    CHECK_FALSE(state.has_value());
}

TEST_CASE("extract_state_header recognises merge conflicts") {
    auto state = mtk::cmds::git::internal::extract_state_header(
        "On branch main\nYou have unmerged paths.\nUnmerged paths:\n");
    REQUIRE(state.has_value());
    CHECK(state->find("merge in progress") != std::string::npos);
    CHECK(state->find("unresolved") != std::string::npos);
}

TEST_CASE("extract_state_header recognises cherry-pick and bisect") {
    auto cp = mtk::cmds::git::internal::extract_state_header(
        "On branch main\nYou are currently cherry-picking commit abc1234.\n");
    REQUIRE(cp.has_value());
    CHECK(*cp == "cherry-pick in progress");

    auto bs = mtk::cmds::git::internal::extract_state_header(
        "On branch main\nYou are currently bisecting, started from rev abc1234.\n");
    REQUIRE(bs.has_value());
    CHECK(*bs == "bisect in progress");
}

TEST_CASE("extract_detached_head finds the detached marker line") {
    auto raw = read_fixture("git_status_plain_detached.txt");
    auto ref = mtk::cmds::git::internal::extract_detached_head(raw);
    REQUIRE(ref.has_value());
    CHECK(ref->find("abc1234") != std::string::npos);
    CHECK(ref->find("HEAD detached at") != std::string::npos);
}

TEST_CASE("extract_detached_head returns nullopt when on a branch") {
    auto ref = mtk::cmds::git::internal::extract_detached_head(
        "On branch main\nnothing to commit, working tree clean\n");
    CHECK_FALSE(ref.has_value());
}

TEST_CASE("filter_status_with_args strips git hint lines and empties") {
    std::string raw =
        "On branch main\n"
        "\n"
        "Changes not staged for commit:\n"
        "  (use \"git add <file>...\" to update what will be committed)\n"
        "  (use \"git restore <file>...\" to discard changes in working directory)\n"
        "        modified:   src/cmds/git.cpp\n"
        "\n"
        "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n";
    auto out = mtk::cmds::git::internal::filter_status_with_args(raw);
    CHECK(out.find("(use \"git add") == std::string::npos);
    CHECK(out.find("(use \"git restore") == std::string::npos);
    CHECK(out.find("modified:   src/cmds/git.cpp") != std::string::npos);
}
