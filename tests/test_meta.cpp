#include <doctest/doctest.h>

#include <iostream>
#include <sstream>
#include <vector>

#include "cmds/meta.hpp"
#include "core/exit_codes.hpp"

namespace {

// Capture both std::cout and std::cerr for the lifetime of the object.
struct StreamCapture {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* prev_out;
    std::streambuf* prev_err;
    StreamCapture()
        : prev_out(std::cout.rdbuf(out.rdbuf())),
          prev_err(std::cerr.rdbuf(err.rdbuf())) {}
    ~StreamCapture() {
        std::cout.rdbuf(prev_out);
        std::cerr.rdbuf(prev_err);
    }
};

}  // namespace

TEST_CASE("run_rewrite: wraps a command a builtin filter handles") {
    StreamCapture cap;
    int rc = mtk::cmds::meta::run_rewrite({"ls", "/tmp"});
    CHECK(rc == 0);
    CHECK(cap.out.str() == "mtk ls /tmp\n");
}

TEST_CASE("run_rewrite: passes through a command no filter handles") {
    StreamCapture cap;
    int rc = mtk::cmds::meta::run_rewrite({"zzqq_no_such_cmd_42", "arg"});
    CHECK(rc == 0);
    CHECK(cap.out.str() == "zzqq_no_such_cmd_42 arg\n");  // unchanged, no "mtk " prefix
}

TEST_CASE("run_rewrite: empty argv is a usage error") {
    StreamCapture cap;
    int rc = mtk::cmds::meta::run_rewrite({});
    CHECK(rc == mtk::core::exit_codes::kUsage);
    CHECK(cap.out.str().empty());  // nothing emitted to stdout on error
}
