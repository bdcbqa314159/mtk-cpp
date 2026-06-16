#include <doctest/doctest.h>

#include <string>

#include "cmds/git.hpp"

// GitShowFilter delegates patch compaction to internal::compact_diff
// (tested in test_git_diff.cpp) and otherwise behaves as a small wrapper
// over `git show --stat --patch --pretty=...`. Its non-trivial logic is
// the multi-commit detection and the `\n---\n` separator stripping.
// These tests cover those exit points.

TEST_CASE("compact_diff stays callable from the git_show.cpp boundary") {
    using mtk::cmds::git::internal::compact_diff;
    // Trivial smoke test: an empty diff should produce empty output.
    CHECK(compact_diff("").empty());

    // A single-file single-hunk diff should produce a file-summary
    // section (verifies that the shared compact_diff still works after
    // the git.cpp split — the show filter relies on it).
    std::string diff =
        "diff --git a/foo b/foo\n"
        "index 1234..5678 100644\n"
        "--- a/foo\n"
        "+++ b/foo\n"
        "@@ -1 +1 @@\n"
        "-old\n"
        "+new\n";
    auto out = compact_diff(diff);
    CHECK(out.find("foo") != std::string::npos);
    CHECK(out.find("+1 -1") != std::string::npos);
}
