#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/toml_filter.hpp"

namespace {

std::string load_cargo_filter() {
    std::ifstream in(std::string(MTK_FIXTURES_DIR) + "/../../filters/cargo.toml");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A realistic `cargo test` run: a Compiling preamble, one compile warning
// (with its --> file:line context), 49 passing tests, 1 failure with a
// panic + assertion, and cargo's own "test result" tally.
std::string sample_cargo_output() {
    std::string s;
    s += "   Compiling libc v0.2.150\n";
    s += "   Compiling proc-macro2 v1.0.70\n";
    s += "   Compiling serde v1.0.193\n";
    s += "   Compiling foo v0.1.0 (/Users/dev/foo)\n";
    s += "warning: unused variable: `x`\n";
    s += "  --> src/lib.rs:10:9\n";
    s += "   |\n";
    s += "10 |     let x = 5;\n";
    s += "   |         ^ help: if this is intentional, prefix it with an underscore: `_x`\n";
    s += "   |\n";
    s += "warning: `foo` (lib) generated 1 warning\n";
    s += "    Finished test [unoptimized + debuginfo] target(s) in 4.13s\n";
    s += "     Running unittests src/lib.rs (target/debug/deps/foo-9ab1c2d3)\n\n";
    s += "running 50 tests\n";
    for (int i = 0; i < 49; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), "test tests::test_case_%03d ... ok\n", i);
        s += line;
    }
    s += "test tests::test_broken ... FAILED\n\n";
    s += "failures:\n\n";
    s += "---- tests::test_broken stdout ----\n";
    s += "thread 'tests::test_broken' panicked at src/lib.rs:42:9:\n";
    s += "assertion `left == right` failed\n";
    s += "  left: 4\n";
    s += "  right: 5\n\n";
    s += "failures:\n";
    s += "    tests::test_broken\n\n";
    s += "test result: FAILED. 49 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out\n";
    return s;
}

}  // namespace

TEST_CASE("cargo filter: keeps warnings/errors/failures, drops chatter, shrinks") {
    auto filters = mtk::core::toml_filter::parse_all(load_cargo_filter());
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];
    CHECK(mtk::core::toml_filter::command_matches(f, "cargo"));

    const std::string input = sample_cargo_output();
    const std::string out = mtk::core::toml_filter::apply(f, input);

    // Signal preserved: the warning + its file:line, the failure, the panic
    // location, and cargo's own tally.
    CHECK(out.find("warning: unused variable") != std::string::npos);
    CHECK(out.find("src/lib.rs:10:9") != std::string::npos);
    CHECK(out.find("test_broken ... FAILED") != std::string::npos);
    CHECK(out.find("panicked at src/lib.rs:42:9") != std::string::npos);
    CHECK(out.find("test result: FAILED. 49 passed; 1 failed") != std::string::npos);

    // Noise gone: build progress and per-test ok lines.
    CHECK(out.find("Compiling") == std::string::npos);
    CHECK(out.find("... ok") == std::string::npos);

    const long pct = 100 - static_cast<long>(out.size() * 100 / input.size());
    std::printf("[cargo spike] bytes_in=%zu bytes_out=%zu savings=%ld%%\n",
                input.size(), out.size(), pct);

    CHECK(out.size() < input.size());
}
