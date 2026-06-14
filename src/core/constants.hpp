#pragma once
#include <string_view>

// Per CR15: format-injection strings and the sentinels we parse against
// must live as co-located siblings. Editing one without the other breaks
// parsing silently; co-location makes the pair visible to reviewers and to
// grep.
namespace mtk::core::constants {

namespace git_log {
    // Injected into `git log` invocations when the user did not set their
    // own --pretty / --format. Produces commit blocks separated by the
    // kCommitSeparator sentinel below.
    //
    // Format: <hash> <subject> (<rel-date>) <author>\n<body>\n---END---
    inline constexpr const char* kPrettyFormat =
        "--pretty=format:%h %s (%ar) <%an>%n%b%n---END---";

    // Sentinel string used to split the raw output back into commit blocks
    // for per-commit filtering. Must match the suffix of kPrettyFormat above.
    inline constexpr std::string_view kCommitSeparator = "---END---";
}  // namespace git_log

}  // namespace mtk::core::constants
