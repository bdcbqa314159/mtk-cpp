#pragma once
#include <cstddef>

// Per CR4: every numeric cap and policy threshold lives here.
// Per-tool sub-namespaces avoid name collisions and keep the policy
// surface for each tool grouped where reviewers can find it.
namespace mtk::core::limits {

// --- git log ---
namespace git_log {
    // Default number of commits shown when the user didn't set -N/-n/--max-count.
    inline constexpr std::size_t kDefaultCommitCount = 10;
    // Soft cap when the user DID set a count (prevent --max-count=100000 from
    // exploding memory; user can always pass --no-compact to bypass).
    inline constexpr std::size_t kUserSetCountCap = 1000;
    // Truncate subject line to this width by default.
    inline constexpr std::size_t kDefaultHeaderWidth = 80;
    // Widen when user passed an explicit count — they wanted detail.
    inline constexpr std::size_t kWideHeaderWidth = 120;
    // Body lines kept per commit before "[+N body lines omitted]".
    inline constexpr std::size_t kMaxBodyLines = 3;
}  // namespace git_log

// --- git diff ---
namespace git_diff {
    // Global cap on lines in the compact diff body. Triggers "[full diff: …]" footer.
    inline constexpr std::size_t kMaxLines = 500;
    // Per-hunk line cap before "... (N lines truncated)" marker.
    inline constexpr std::size_t kMaxHunkLines = 100;
}  // namespace git_diff

// --- grep ---
namespace grep {
    // Truncate each match line to this many chars (centered on the pattern).
    inline constexpr std::size_t kMaxLineLen = 80;
    // Stop emitting after this many total matches.
    inline constexpr std::size_t kMaxResults = 200;
    // Per-file cap on matches shown.
    inline constexpr std::size_t kPerFile = 25;
    // Compact long file paths to "first/.../parent/file" when over this length.
    inline constexpr std::size_t kCompactPathThreshold = 50;
    // Only compact if path has at least this many segments.
    inline constexpr std::size_t kCompactPathMinSegments = 4;
}  // namespace grep

// --- ls ---
namespace ls {
    // Show top-N extensions in the summary line before "+M more".
    inline constexpr std::size_t kExtSummaryMaxCount = 5;
}  // namespace ls

// --- tee / audit ---
namespace tee {
    // Slug derived from command name for tee filenames; truncated to keep
    // filesystem-friendly lengths and avoid path-length issues.
    inline constexpr std::size_t kSlugMaxLen = 40;
}  // namespace tee

// --- formatting / human-readable sizes ---
namespace fmt {
    inline constexpr std::size_t kHumanKB = 1024;
    inline constexpr std::size_t kHumanMB = 1024 * 1024;
}  // namespace fmt

// --- subprocess capture bounds (A6) ---
namespace capture {
    // Hard cap on total captured stdout+stderr bytes. On overflow the
    // child is terminated and the outcome is flagged truncated.
    inline constexpr std::size_t kMaxBytes = 50 * 1024 * 1024;  // 50 MiB
    // Hard cap on subprocess wall-clock. On timeout the child gets
    // SIGTERM, waits signal::kGraceMs, then SIGKILL.
    inline constexpr long kTimeoutMs = 30000;
}  // namespace capture

// --- signal handling (A11) ---
namespace signal_ {
    // Grace period between SIGTERM and SIGKILL when terminating a child.
    inline constexpr long kGraceMs = 2000;
}  // namespace signal_

}  // namespace mtk::core::limits
