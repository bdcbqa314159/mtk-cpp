#include <doctest/doctest.h>

#include <string>

#include "core/color.hpp"

using mtk::core::color::resolve_policy;

TEST_CASE("resolve_policy: NO_COLOR (non-empty) wins over everything") {
    CHECK_FALSE(resolve_policy("1", "always", true));
    CHECK_FALSE(resolve_policy("anything", nullptr, true));
    CHECK_FALSE(resolve_policy("1", "always", false));
}

TEST_CASE("resolve_policy: empty NO_COLOR is ignored (falls through)") {
    CHECK(resolve_policy("", "always", false));   // -> MTK_COLOR=always
    CHECK(resolve_policy("", nullptr, true));      // -> tty
    CHECK_FALSE(resolve_policy("", nullptr, false));
}

TEST_CASE("resolve_policy: MTK_COLOR never/always override tty") {
    CHECK_FALSE(resolve_policy(nullptr, "never", true));
    CHECK(resolve_policy(nullptr, "always", false));
}

TEST_CASE("resolve_policy: auto / unset / unknown follow tty") {
    CHECK(resolve_policy(nullptr, nullptr, true));
    CHECK_FALSE(resolve_policy(nullptr, nullptr, false));
    CHECK(resolve_policy(nullptr, "auto", true));
    CHECK_FALSE(resolve_policy(nullptr, "auto", false));
    CHECK(resolve_policy(nullptr, "garbage", true));   // unknown value -> tty
    CHECK_FALSE(resolve_policy(nullptr, "garbage", false));
}

TEST_CASE("color::strip removes CSI escapes and is idempotent") {
    const std::string colored = "\x1b[31mred\x1b[0m \x1b[1mbold\x1b[0m";
    auto once = mtk::core::color::strip(colored);
    CHECK(once == "red bold");
    CHECK(mtk::core::color::strip(once) == once);   // idempotent
    CHECK(mtk::core::color::strip("plain text") == "plain text");  // no-op fast path
}
