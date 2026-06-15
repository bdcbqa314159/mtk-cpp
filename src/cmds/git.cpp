#include "cmds/git.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

#include "core/constants.hpp"
#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/filter.hpp"
#include "core/limits.hpp"
#include "core/run_context.hpp"
#include "core/tee.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::git {

namespace internal {

namespace {

using mtk::core::utils::starts_with;

bool is_short_count_flag(const std::string& s) {
    if (s.size() < 2 || s[0] != '-') return false;
    if (!std::isdigit(static_cast<unsigned char>(s[1]))) return false;
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

}  // namespace

LogOptions detect_log_options(const std::vector<std::string>& args) {
    LogOptions opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--") break;
        if (starts_with(a, "--pretty") || starts_with(a, "--format")) {
            opts.user_set_format = true;
        }
        if (starts_with(a, "--max-count")) opts.user_set_count = true;
        if (a == "-n" && i + 1 < args.size()) opts.user_set_count = true;
        if (is_short_count_flag(a)) opts.user_set_count = true;
    }
    return opts;
}

std::string filter_log_output(std::string_view raw,
                              std::size_t max_commits,
                              std::size_t header_width,
                              std::size_t max_body_lines) {
    const std::string sep(mtk::core::constants::git_log::kCommitSeparator);

    std::vector<std::string> commits;
    std::size_t pos = 0;
    while (pos < raw.size() && commits.size() < max_commits) {
        auto next = raw.find(sep, pos);
        std::string_view chunk;
        if (next == std::string_view::npos) {
            chunk = raw.substr(pos);
            pos = raw.size();
        } else {
            chunk = raw.substr(pos, next - pos);
            pos = next + sep.size();
            if (pos < raw.size() && raw[pos] == '\n') ++pos;
        }

        auto lines = mtk::core::utils::split_lines_view(chunk);
        while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
        if (lines.empty()) continue;

        std::ostringstream commit;
        commit << mtk::core::utils::truncate(lines.front(), header_width);

        std::size_t body_emitted = 0;
        std::size_t body_skipped = 0;
        for (std::size_t i = 1; i < lines.size(); ++i) {
            const auto& l = lines[i];
            if (l.empty()) continue;
            if (starts_with(l, "Signed-off-by:")) continue;
            if (starts_with(l, "Co-authored-by:")) continue;
            if (body_emitted < max_body_lines) {
                commit << "\n  " << mtk::core::utils::truncate(l, header_width);
                ++body_emitted;
            } else {
                ++body_skipped;
            }
        }
        if (body_skipped > 0) {
            commit << "\n  [+" << body_skipped << " body lines omitted]";
        }
        commits.push_back(commit.str());
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < commits.size(); ++i) {
        out << commits[i];
        if (i + 1 < commits.size()) out << "\n\n";
    }
    return out.str();
}

// --- git status helpers ---

bool uses_compact_status_path(const std::vector<std::string>& args) {
    if (args.empty()) return true;
    bool saw_branch = false;
    for (const auto& a : args) {
        if (a == "-b" || a == "--branch") {
            saw_branch = true;
        } else if (a == "-sb" || a == "-bs") {
            return true;
        } else if (a == "-s" || a == "--short") {
            continue;
        } else {
            return false;
        }
    }
    return saw_branch;
}

namespace {

const std::vector<std::string>& rebase_indicators() {
    static const std::vector<std::string> v = {
        "rebase in progress",
        "You are currently rebasing",
        "You are currently editing",
        "You are currently splitting",
        "Last command done",
        "Next command to do",
        "No commands remaining",
    };
    return v;
}

const std::vector<std::string>& state_stoppers() {
    static const std::vector<std::string> v = {
        "Changes to be committed:",
        "Changes not staged for commit:",
        "Untracked files:",
        "Unmerged paths:",
        "no changes added to commit",
        "nothing to commit",
        "nothing added to commit",
    };
    return v;
}

bool line_contains_any(const std::string& line, const std::vector<std::string>& needles) {
    for (const auto& n : needles) {
        if (line.find(n) != std::string::npos) return true;
    }
    return false;
}

std::optional<std::string> detect_status_state(const std::string& line) {
    if (line.find("All conflicts fixed but you are still merging") != std::string::npos)
        return std::string("merge in progress. no conflicts");
    if (line.find("You have unmerged paths") != std::string::npos)
        return std::string("merge in progress. unresolved conflicts");
    if (line.find("You are currently cherry-picking") != std::string::npos)
        return std::string("cherry-pick in progress");
    if (line.find("You are currently reverting") != std::string::npos)
        return std::string("revert in progress");
    if (line.find("You are currently bisecting") != std::string::npos)
        return std::string("bisect in progress");
    if (line.find("You are in the middle of an am session") != std::string::npos)
        return std::string("am session in progress");
    if (line.find("You are in a sparse checkout") != std::string::npos)
        return std::string("sparse checkout enabled");
    if (line_contains_any(line, rebase_indicators()))
        return std::string("rebase in progress");
    return std::nullopt;
}

using mtk::core::utils::trim_copy;

}  // namespace

std::optional<std::string> extract_state_header(std::string_view plain) {
    for (const auto& raw_line : mtk::core::utils::split_lines_view(plain)) {
        std::string stripped = trim_copy(raw_line);
        for (const auto& stopper : state_stoppers()) {
            if (starts_with(stripped, stopper)) return std::nullopt;
        }
        if (auto state = detect_status_state(stripped)) return state;
    }
    return std::nullopt;
}

std::optional<std::string> extract_detached_head(std::string_view plain) {
    for (const auto& raw_line : mtk::core::utils::split_lines_view(plain)) {
        std::string stripped = trim_copy(raw_line);
        if (starts_with(stripped, "HEAD detached ")) return stripped;
    }
    return std::nullopt;
}

std::string format_status_output(std::string_view porcelain,
                                 std::optional<std::string> detached_ref) {
    std::vector<std::string> lines;
    for (auto& raw_line : mtk::core::utils::split_lines(porcelain)) {
        if (!trim_copy(raw_line).empty()) lines.push_back(std::move(raw_line));
    }
    if (lines.empty()) return "Clean working tree";

    std::vector<std::string> out;
    const auto& first = lines.front();
    if (starts_with(first, "##")) {
        std::string branch = first.substr(2);
        if (!branch.empty() && branch.front() == ' ') branch.erase(0, 1);
        const std::string& display = detached_ref ? *detached_ref : branch;
        out.push_back("* " + display);
    } else {
        out.push_back(first);
    }
    for (std::size_t i = 1; i < lines.size(); ++i) out.push_back(lines[i]);
    if (lines.size() == 1 && starts_with(lines[0], "##")) {
        out.emplace_back("clean — nothing to commit");
    }
    return mtk::core::utils::join_lines(out);
}

std::string filter_status_with_args(std::string_view raw) {
    std::vector<std::string> result;
    for (auto& line : mtk::core::utils::split_lines(raw)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty()) continue;
        if (starts_with(trimmed, "(use \"git") ||
            starts_with(trimmed, "(create/copy files") ||
            trimmed.find("(use \"git add") != std::string::npos ||
            trimmed.find("(use \"git restore") != std::string::npos) {
            continue;
        }
        if (trimmed.find("nothing to commit") != std::string::npos &&
            trimmed.find("working tree clean") != std::string::npos) {
            result.push_back(trimmed);
            break;
        }
        result.push_back(line);
    }
    if (result.empty()) return "ok";
    return mtk::core::utils::join_lines(result);
}

// --- git diff ---

DiffOptions detect_diff_options(const std::vector<std::string>& args) {
    DiffOptions opts;
    for (const auto& a : args) {
        if (a == "--stat" || a == "--numstat" || a == "--shortstat") opts.wants_stat = true;
        if (a == "--no-compact") opts.wants_no_compact = true;
    }
    return opts;
}

namespace {

std::string extract_b_path(std::string_view diff_git_line) {
    auto pos = diff_git_line.find(" b/");
    if (pos == std::string_view::npos) return "unknown";
    return std::string(diff_git_line.substr(pos + 3));
}

}  // namespace

std::string compact_diff(std::string_view diff,
                         std::size_t max_lines,
                         std::size_t max_hunk_lines) {
    std::vector<std::string> result;
    std::string current_file;
    std::size_t added = 0;
    std::size_t removed = 0;
    bool in_hunk = false;
    std::size_t hunk_shown = 0;
    std::size_t hunk_skipped = 0;
    bool was_truncated = false;

    auto flush_hunk_truncation = [&] {
        if (hunk_skipped > 0) {
            result.push_back("  ... (" + std::to_string(hunk_skipped) + " lines truncated)");
            was_truncated = true;
            hunk_skipped = 0;
        }
    };

    auto flush_file_summary = [&] {
        if (!current_file.empty() && (added > 0 || removed > 0)) {
            result.push_back("  +" + std::to_string(added) + " -" + std::to_string(removed));
        }
    };

    for (const auto& line : mtk::core::utils::split_lines_view(diff)) {
        if (starts_with(line, "diff --git")) {
            flush_hunk_truncation();
            flush_file_summary();
            current_file = extract_b_path(line);
            result.push_back("\n" + current_file);
            added = 0;
            removed = 0;
            in_hunk = false;
            hunk_shown = 0;
        } else if (starts_with(line, "@@")) {
            flush_hunk_truncation();
            in_hunk = true;
            hunk_shown = 0;
            result.push_back("  " + std::string(line));
        } else if (in_hunk) {
            if (starts_with(line, "+") && !starts_with(line, "+++")) {
                ++added;
                if (hunk_shown < max_hunk_lines) {
                    result.push_back("  " + std::string(line));
                    ++hunk_shown;
                } else {
                    ++hunk_skipped;
                }
            } else if (starts_with(line, "-") && !starts_with(line, "---")) {
                ++removed;
                if (hunk_shown < max_hunk_lines) {
                    result.push_back("  " + std::string(line));
                    ++hunk_shown;
                } else {
                    ++hunk_skipped;
                }
            } else if (hunk_shown < max_hunk_lines && !starts_with(line, "\\")) {
                if (hunk_shown > 0) {
                    result.push_back("  " + std::string(line));
                    ++hunk_shown;
                }
            }
        }

        if (result.size() >= max_lines) {
            result.push_back("\n... (more changes truncated)");
            was_truncated = true;
            break;
        }
    }

    flush_hunk_truncation();
    flush_file_summary();

    if (was_truncated) {
        result.push_back("[full diff: mtk git diff --no-compact]");
    }

    return mtk::core::utils::join_lines(result);
}

}  // namespace internal

namespace {

std::vector<std::string> build_log_argv(const std::vector<std::string>& user_args,
                                        const internal::LogOptions& opts,
                                        std::size_t default_count) {
    namespace gl = mtk::core::constants::git_log;
    std::vector<std::string> argv = {"git", "log"};
    if (!opts.user_set_count) {
        argv.push_back("-" + std::to_string(default_count));
    }
    if (!opts.user_set_format) {
        argv.push_back(gl::kPrettyFormat);
    }
    for (const auto& a : user_args) argv.push_back(a);
    return argv;
}

bool is_blob_show_arg(const std::string& a) {
    return !a.empty() && a[0] != '-' && a.find(':') != std::string::npos;
}

namespace ex = mtk::core::exec;
using mtk::core::DispatchTokenPtr;

// --- GitLogFilter ---
class GitLogFilter final : public mtk::core::Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "git_log"; }
    [[nodiscard]] std::string_view source() const noexcept override { return "builtin"; }
    [[nodiscard]] std::string_view literal_first_token() const noexcept override { return "git"; }

    [[nodiscard]] std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override {
        if (argv.size() < 2 || argv[0] != "git" || argv[1] != "log") return std::nullopt;
        return DispatchTokenPtr{};
    }

    [[nodiscard]] ex::ExecOutcome
    run(DispatchTokenPtr, const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override {
        namespace lim = mtk::core::limits::git_log;
        std::vector<std::string> user_args(argv.begin() + 2, argv.end());
        auto opts = internal::detect_log_options(user_args);
        auto cmd_argv = build_log_argv(user_args, opts, lim::kDefaultCommitCount);

        auto outcome = ctx.capture(cmd_argv);
        const auto* ran = ctx.as_ran(outcome);
        if (!ran) return outcome;

        ctx.tee_on_failure(outcome, "git_log");

        std::string filtered;
        if (opts.user_set_format) {
            filtered = ran->stdout_data;
        } else {
            try {
                filtered = internal::filter_log_output(
                    ran->stdout_data,
                    opts.user_set_count ? lim::kUserSetCountCap : lim::kDefaultCommitCount,
                    opts.user_set_count ? lim::kWideHeaderWidth : lim::kDefaultHeaderWidth,
                    lim::kMaxBodyLines);
            } catch (const std::exception&) {
                filtered = ran->stdout_data;  // A4 fallback
            }
        }
        return ex::Ran{std::move(filtered), ran->stderr_data, ran->exit_code,
                       ran->truncated, ran->timed_out, ran->killed_by_signal};
    }
};

// --- GitStatusFilter ---
class GitStatusFilter final : public mtk::core::Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "git_status"; }
    [[nodiscard]] std::string_view source() const noexcept override { return "builtin"; }
    [[nodiscard]] std::string_view literal_first_token() const noexcept override { return "git"; }

    [[nodiscard]] std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override {
        if (argv.size() < 2 || argv[0] != "git" || argv[1] != "status") return std::nullopt;
        return DispatchTokenPtr{};
    }

    [[nodiscard]] ex::ExecOutcome
    run(DispatchTokenPtr, const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override {
        std::vector<std::string> user_args(argv.begin() + 2, argv.end());

        if (!internal::uses_compact_status_path(user_args)) {
            std::vector<std::string> cmd_argv = {"git", "status"};
            cmd_argv.insert(cmd_argv.end(), user_args.begin(), user_args.end());
            auto outcome = ctx.capture(cmd_argv);
            const auto* ran = ctx.as_ran(outcome);
            if (!ran) return outcome;
            if (!ran->clean()) return outcome;

            std::string filtered;
            try {
                filtered = internal::filter_status_with_args(ran->stdout_data);
            } catch (const std::exception&) {
                filtered = ran->stdout_data;
            }
            return ex::Ran{std::move(filtered), std::string{}, 0,
                           ran->truncated, ran->timed_out, ran->killed_by_signal};
        }

        std::vector<std::string> plain_argv = {"git", "status"};
        plain_argv.insert(plain_argv.end(), user_args.begin(), user_args.end());
        auto plain_out = ctx.capture(plain_argv);

        std::vector<std::string> porcelain_argv = {"git", "status", "--porcelain", "-b"};
        auto porcelain_out = ctx.capture(porcelain_argv);
        const auto* porcelain = ctx.as_ran(porcelain_out);
        if (!porcelain) return porcelain_out;

        if (!porcelain->stderr_data.empty() &&
            porcelain->stderr_data.find("not a git repository") != std::string::npos) {
            return ex::Ran{std::string{}, "Not a git repository\n",
                           porcelain->exit_code != 0 ? porcelain->exit_code : 128,
                           false, false, 0};
        }

        const auto* plain = ctx.as_ran(plain_out);
        std::string_view plain_view = (plain && plain->clean())
            ? std::string_view(plain->stdout_data) : std::string_view{};

        auto detached = internal::extract_detached_head(plain_view);
        auto formatted = internal::format_status_output(porcelain->stdout_data, detached);
        auto state = internal::extract_state_header(plain_view);

        std::string out;
        if (state) { out += *state; out += '\n'; }
        out += formatted;
        return ex::Ran{std::move(out), std::string{}, porcelain->exit_code,
                       porcelain->truncated, porcelain->timed_out,
                       porcelain->killed_by_signal};
    }
};

// --- GitDiffFilter ---
class GitDiffFilter final : public mtk::core::Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "git_diff"; }
    [[nodiscard]] std::string_view source() const noexcept override { return "builtin"; }
    [[nodiscard]] std::string_view literal_first_token() const noexcept override { return "git"; }

    [[nodiscard]] std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override {
        if (argv.size() < 2 || argv[0] != "git" || argv[1] != "diff") return std::nullopt;
        return DispatchTokenPtr{};
    }

    [[nodiscard]] ex::ExecOutcome
    run(DispatchTokenPtr, const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override {
        std::vector<std::string> user_args(argv.begin() + 2, argv.end());
        auto opts = internal::detect_diff_options(user_args);

        std::vector<std::string> args_no_compact;
        args_no_compact.reserve(user_args.size());
        for (const auto& a : user_args) {
            if (a != "--no-compact") args_no_compact.push_back(a);
        }

        if (opts.wants_stat || opts.wants_no_compact) {
            std::vector<std::string> cmd_argv = {"git", "diff"};
            cmd_argv.insert(cmd_argv.end(), args_no_compact.begin(), args_no_compact.end());
            return ctx.capture(cmd_argv);  // raw passthrough of git diff output
        }

        // Per perf critic P8 (Round D): single invocation. `git diff
        // --stat --patch` outputs the diffstat block, a blank line, then
        // the patch — all in one shot. Halves fork+exec cost vs the
        // previous two-spawn pattern (one for --stat, one for the patch).
        std::vector<std::string> diff_argv = {"git", "diff", "--stat", "--patch"};
        diff_argv.insert(diff_argv.end(), args_no_compact.begin(), args_no_compact.end());
        auto diff_out = ctx.capture(diff_argv);
        const auto* diff = ctx.as_ran(diff_out);
        if (!diff) return diff_out;
        if (!diff->clean()) return diff_out;

        // Split at the first `diff --git` line: everything before is the
        // --stat block; everything from that line onward is the patch.
        std::string_view full(diff->stdout_data);
        auto patch_start = full.find("diff --git");

        std::string out;
        if (patch_start == std::string_view::npos) {
            // No patch — either no changes or stat-only output. Pass the
            // stat through unchanged.
            out = std::string(full);
        } else {
            std::string_view stat_block = full.substr(0, patch_start);
            std::string_view patch = full.substr(patch_start);

            // Strip trailing blank lines on the stat block — git inserts a
            // blank separator that we don't want to keep ahead of "Changes:".
            while (!stat_block.empty() &&
                   (stat_block.back() == '\n' || stat_block.back() == ' ')) {
                stat_block.remove_suffix(1);
            }
            if (!stat_block.empty()) {
                out.assign(stat_block);
                out += '\n';
            }
            out += "\nChanges:";
            std::string compacted;
            try {
                compacted = internal::compact_diff(patch);
            } catch (const std::exception&) {
                compacted = std::string(patch);
            }
            out += compacted;
        }
        return ex::Ran{std::move(out), std::string{}, diff->exit_code,
                       diff->truncated, diff->timed_out, diff->killed_by_signal};
    }
};

// --- GitShowFilter ---
class GitShowFilter final : public mtk::core::Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "git_show"; }
    [[nodiscard]] std::string_view source() const noexcept override { return "builtin"; }
    [[nodiscard]] std::string_view literal_first_token() const noexcept override { return "git"; }

    [[nodiscard]] std::optional<DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override {
        if (argv.size() < 2 || argv[0] != "git" || argv[1] != "show") return std::nullopt;
        return DispatchTokenPtr{};
    }

    [[nodiscard]] ex::ExecOutcome
    run(DispatchTokenPtr, const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override {
        std::vector<std::string> user_args(argv.begin() + 2, argv.end());
        bool wants_stat_only = false, wants_format = false, wants_blob = false;
        for (const auto& a : user_args) {
            if (a == "--stat" || a == "--numstat" || a == "--shortstat") wants_stat_only = true;
            if (a.rfind("--pretty", 0) == 0 || a.rfind("--format", 0) == 0) wants_format = true;
            if (is_blob_show_arg(a)) wants_blob = true;
        }

        if (wants_stat_only || wants_format || wants_blob) {
            std::vector<std::string> cmd_argv = {"git", "show"};
            cmd_argv.insert(cmd_argv.end(), user_args.begin(), user_args.end());
            return ctx.capture(cmd_argv);
        }

        // Per perf critic P8 (Round D): single invocation. `%n` at the
        // end of the pretty format ensures the summary line is newline-
        // terminated before the --stat block; `--stat --patch` then emits
        // stat-block then patch (with a blank line between). Saves
        // 2 fork+exec round-trips vs the previous three-spawn pattern.
        std::vector<std::string> show_argv = {"git", "show", "--stat", "--patch",
                                              "--pretty=format:%h %s (%ar) <%an>%n"};
        show_argv.insert(show_argv.end(), user_args.begin(), user_args.end());
        auto show_out = ctx.capture(show_argv);
        const auto* show = ctx.as_ran(show_out);
        if (!show) return show_out;
        if (!show->clean()) {
            return ex::Ran{std::string{}, show->stderr_data, show->exit_code,
                           show->truncated, show->timed_out, show->killed_by_signal};
        }

        // Split at the first `diff --git` line: everything before is the
        // summary line + --stat block; everything from there is the patch.
        std::string_view full(show->stdout_data);
        auto patch_start = full.find("diff --git");

        std::string out;
        // Git inserts a literal `---\n` commit-boundary separator between
        // the pretty-format prefix and the --stat block. Strip it once
        // (single occurrence guaranteed for a one-commit `git show`).
        auto strip_git_separator = [](std::string& s) {
            auto pos = s.find("\n---\n");
            if (pos != std::string::npos) s.erase(pos, 4);  // leave the trailing \n
        };

        if (patch_start == std::string_view::npos) {
            // No patch present (e.g. empty commit). Pass through.
            out.assign(full);
            strip_git_separator(out);
            if (!out.empty() && out.back() != '\n') out += '\n';
        } else {
            std::string_view summary_stat = full.substr(0, patch_start);
            std::string_view patch = full.substr(patch_start);
            out.assign(summary_stat);
            strip_git_separator(out);
            if (!out.empty() && out.back() != '\n') out += '\n';

            std::string compacted;
            try {
                compacted = internal::compact_diff(patch);
            } catch (const std::exception&) {
                compacted.assign(patch);
            }
            out += compacted;
            if (!compacted.empty() && compacted.back() != '\n') out += '\n';
        }
        return ex::Ran{std::move(out), std::string{}, 0,
                       show->truncated, show->timed_out, show->killed_by_signal};
    }
};

}  // namespace

void register_builtins(mtk::core::Registry& reg) {
    namespace ct = mtk::core;
    reg.register_filter(std::make_unique<GitLogFilter>(),    ct::Tier::Builtin, true);
    reg.register_filter(std::make_unique<GitStatusFilter>(), ct::Tier::Builtin, true);
    reg.register_filter(std::make_unique<GitDiffFilter>(),   ct::Tier::Builtin, true);
    reg.register_filter(std::make_unique<GitShowFilter>(),   ct::Tier::Builtin, true);
}

}  // namespace mtk::cmds::git
