#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/registry.hpp"

namespace mtk::cmds::git {

// Registers GitLogFilter, GitStatusFilter, GitDiffFilter, GitShowFilter
// into the given registry at Tier::Builtin with is_final=true.
void register_builtins(mtk::core::Registry& reg);

namespace internal {

struct LogOptions {
    bool user_set_format = false;
    bool user_set_count = false;
};

LogOptions detect_log_options(const std::vector<std::string>& args);

std::string filter_log_output(std::string_view raw,
                              std::size_t max_commits,
                              std::size_t header_width,
                              std::size_t max_body_lines);

// --- git status ---

bool uses_compact_status_path(const std::vector<std::string>& args);

std::optional<std::string> extract_state_header(std::string_view plain);
std::optional<std::string> extract_detached_head(std::string_view plain);

std::string format_status_output(std::string_view porcelain,
                                 std::optional<std::string> detached_ref = std::nullopt);

std::string filter_status_with_args(std::string_view raw);

// --- git diff ---

struct DiffOptions {
    bool wants_stat = false;
    bool wants_no_compact = false;
};

DiffOptions detect_diff_options(const std::vector<std::string>& args);

std::string compact_diff(std::string_view diff,
                         std::size_t max_lines = 500,
                         std::size_t max_hunk_lines = 100);

}  // namespace internal

}  // namespace mtk::cmds::git
