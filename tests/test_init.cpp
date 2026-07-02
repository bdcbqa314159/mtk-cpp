#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cmds/init.hpp"

namespace fs = std::filesystem;

namespace {

struct CwdGuard {
    fs::path saved;
    explicit CwdGuard(const fs::path& to) : saved(fs::current_path()) {
        fs::current_path(to);
    }
    ~CwdGuard() {
        std::error_code ec;
        fs::current_path(saved, ec);
    }
};

struct CoutSilencer {
    std::ostringstream sink;
    std::streambuf* prev;
    CoutSilencer() : prev(std::cout.rdbuf(sink.rdbuf())) {}
    ~CoutSilencer() { std::cout.rdbuf(prev); }
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t count_occurrences(const std::string& hay, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

TEST_CASE("run_init copilot writes hook + instructions and upserts idempotently") {
    const auto tmp = fs::temp_directory_path() / "mtk_init_copilot_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    {
        CwdGuard cwd(tmp);
        CoutSilencer hush;
        CHECK(mtk::cmds::init::run_init({"copilot"}) == 0);
    }

    const auto hook = tmp / ".github" / "hooks" / "mtk-rewrite.json";
    const auto instr = tmp / ".github" / "copilot-instructions.md";
    REQUIRE(fs::exists(hook));
    REQUIRE(fs::exists(instr));
    CHECK(read_file(hook).find("hook copilot") != std::string::npos);

    const std::string marker = "<!-- mtk-instructions v1 -->";
    CHECK(count_occurrences(read_file(instr), marker) == 1);

    // Running again must REPLACE the marker block, not append a second copy.
    {
        CwdGuard cwd(tmp);
        CoutSilencer hush;
        CHECK(mtk::cmds::init::run_init({"copilot"}) == 0);
    }
    CHECK(count_occurrences(read_file(instr), marker) == 1);

    fs::remove_all(tmp, ec);
}

TEST_CASE("run_init with unknown agent is a usage error") {
    CoutSilencer hush;  // the help/usage text is irrelevant here
    std::ostringstream errsink;
    auto* prev = std::cerr.rdbuf(errsink.rdbuf());
    int rc = mtk::cmds::init::run_init({"nonexistent_agent"});
    std::cerr.rdbuf(prev);
    CHECK(rc != 0);
}
