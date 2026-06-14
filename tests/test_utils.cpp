#include <doctest/doctest.h>

#include "core/utils.hpp"

TEST_CASE("strip_ansi removes SGR escape sequences") {
    auto out = mtk::core::utils::strip_ansi("\x1b[32mok\x1b[0m fail\x1b[1;31m!\x1b[0m");
    CHECK(out == "ok fail!");
}

TEST_CASE("truncate respects UTF-8 boundaries") {
    auto a = mtk::core::utils::truncate("hello world", 5);
    CHECK(a == "he...");

    auto b = mtk::core::utils::truncate("short", 100);
    CHECK(b == "short");
}

TEST_CASE("split_lines does not duplicate trailing empty") {
    auto v = mtk::core::utils::split_lines("a\nb\nc\n");
    CHECK(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[2] == "c");
}

TEST_CASE("count_tokens counts whitespace-separated words") {
    CHECK(mtk::core::utils::count_tokens("hello world") == 2);
    CHECK(mtk::core::utils::count_tokens("   ") == 0);
    CHECK(mtk::core::utils::count_tokens("a\nb\tc d") == 4);
}
