#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cmds/ls.hpp"

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

TEST_CASE("parse_args recognises short and long flag combinations") {
    using mtk::cmds::ls::internal::parse_args;
    CHECK(parse_args({"-a"}).show_all);
    CHECK(parse_args({"--all"}).show_all);
    CHECK(parse_args({"-l"}).show_long);
    CHECK(parse_args({"-la"}).show_all);
    CHECK(parse_args({"-la"}).show_long);
    CHECK(parse_args({"-g"}).show_long);
    CHECK(parse_args({"--full-time"}).show_long);
    CHECK_FALSE(parse_args({"-h"}).show_long);
    CHECK_FALSE(parse_args({"path"}).show_all);
}

TEST_CASE("human_size formats bytes/K/M") {
    using mtk::cmds::ls::internal::human_size;
    CHECK(human_size(0) == "0B");
    CHECK(human_size(500) == "500B");
    CHECK(human_size(1024) == "1.0K");
    CHECK(human_size(1234) == "1.2K");
    CHECK(human_size(1048576) == "1.0M");
}

TEST_CASE("perms_to_octal handles common modes and special bits") {
    using mtk::cmds::ls::internal::perms_to_octal;
    CHECK(*perms_to_octal("-rw-r--r--") == "644");
    CHECK(*perms_to_octal("-rwxr-xr-x") == "755");
    CHECK(*perms_to_octal("drwxr-xr-x") == "755");
    CHECK(*perms_to_octal("-rw-------") == "600");
    CHECK(*perms_to_octal("drwxrwxrwt") == "1777");
    CHECK(*perms_to_octal("-rwsr-xr-x") == "4755");
    CHECK_FALSE(perms_to_octal("short").has_value());
}

TEST_CASE("parse_ls_line extracts perms / size / name with locale-stable anchor") {
    using mtk::cmds::ls::internal::parse_ls_line;
    auto e = parse_ls_line("-rw-r--r--  1 user staff 1234 Jun 14 09:00 file.txt");
    REQUIRE(e.has_value());
    CHECK(e->type == '-');
    CHECK(e->perms == "-rw-r--r--");
    CHECK(e->size == 1234);
    CHECK(e->name == "file.txt");

    auto sym = parse_ls_line("lrwxr-xr-x  1 user staff 12 Jun 14 09:00 link -> ../target");
    REQUIRE(sym.has_value());
    CHECK(sym->type == 'l');
    CHECK(sym->name == "link -> ../target");
}

TEST_CASE("compact_ls drops noise dirs, classifies dirs vs files, sums extensions") {
    auto raw = read_fixture("ls_la_raw.txt");
    auto r = mtk::cmds::ls::internal::compact_ls(raw, /*show_all=*/false, /*show_long=*/false);
    CHECK(r.parsed_count > 0);
    CHECK(r.entries.find("src/") != std::string::npos);
    CHECK(r.entries.find("tests/") != std::string::npos);
    CHECK(r.entries.find(".git/") == std::string::npos);
    CHECK(r.entries.find("node_modules") == std::string::npos);
    CHECK(r.entries.find("CMakeLists.txt") != std::string::npos);
    CHECK(r.entries.find("build.log") != std::string::npos);
    CHECK(r.entries.find("1.0M") != std::string::npos);
    CHECK(r.summary.find("Summary:") != std::string::npos);
    CHECK(r.summary.find(".md") != std::string::npos);
}

TEST_CASE("compact_ls with show_all keeps noise dirs visible") {
    auto raw = read_fixture("ls_la_raw.txt");
    auto r = mtk::cmds::ls::internal::compact_ls(raw, /*show_all=*/true, /*show_long=*/false);
    CHECK(r.entries.find(".git/") != std::string::npos);
    CHECK(r.entries.find("node_modules/") != std::string::npos);
}

TEST_CASE("compact_ls with show_long emits octal perms") {
    auto raw = read_fixture("ls_la_raw.txt");
    auto r = mtk::cmds::ls::internal::compact_ls(raw, /*show_all=*/false, /*show_long=*/true);
    CHECK(r.entries.find("755  src/") != std::string::npos);
    CHECK(r.entries.find("644  CMakeLists.txt") != std::string::npos);
    CHECK(r.entries.find("755  build.sh") != std::string::npos);
}

TEST_CASE("compact_ls empty-input emits (empty)") {
    auto r = mtk::cmds::ls::internal::compact_ls("total 0\n", false, false);
    CHECK(r.entries == "(empty)\n");
    CHECK(r.summary.empty());
}

// Audit regression test: a directory containing only noise dirs (node_modules
// etc.) used to show "(empty)\n" then silently dropped to empty stdout after
// the Phase 4 compact_ls decomposition. This locks the all-noise case in.
TEST_CASE("compact_ls all-noise directory emits (empty)") {
    using mtk::cmds::ls::internal::compact_ls;
    std::string raw =
        "total 16\n"
        "drwxr-xr-x  10 user group 320 Jan  1 12:00 node_modules\n"
        "drwxr-xr-x   5 user group 160 Jan  1 12:00 .git\n"
        "drwxr-xr-x   3 user group  96 Jan  1 12:00 __pycache__\n";
    auto r = compact_ls(raw, /*show_all=*/false, /*show_long=*/false);
    CHECK(r.entries == "(empty)\n");
    CHECK(r.summary.empty());

    // With show_all=true, the same dirs surface.
    auto r2 = compact_ls(raw, /*show_all=*/true, /*show_long=*/false);
    CHECK(r2.entries.find("node_modules/") != std::string::npos);
    CHECK(r2.entries.find(".git/") != std::string::npos);
}
