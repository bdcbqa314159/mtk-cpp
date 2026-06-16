#include "cmds/git.hpp"

#include <string>

#include "core/exec.hpp"
#include "core/filter.hpp"
#include "core/run_context.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::git {

namespace internal {

namespace {

using mtk::core::utils::starts_with;

std::string extract_b_path(std::string_view diff_git_line) {
    auto pos = diff_git_line.find(" b/");
    if (pos == std::string_view::npos) return "unknown";
    return std::string(diff_git_line.substr(pos + 3));
}

// Method-Object refactor of compact_diff (per audit). Each diff line
// dispatches to a named mode handler. State (current file, +/-
// counters, hunk position) lives on the object, not as local vars +
// captured lambdas. Two ordering touches (C4, P8) made the original
// state-machine fragile enough to deserve naming.
//
// Per perf critic P3 (Round F): output accumulates into a single
// std::string rather than vector<string>+join_lines. Saves one heap
// alloc per emitted line plus the join_lines ostringstream pass; on a
// 500-line diff that's a measurable ~20-50 µs.
class DiffCompactor {
public:
    DiffCompactor(std::size_t max_lines, std::size_t max_hunk_lines) noexcept
        : max_lines_(max_lines), max_hunk_lines_(max_hunk_lines) {}

    void feed(std::string_view diff) {
        for (const auto& line : mtk::core::utils::split_lines_view(diff)) {
            if (starts_with(line, "diff --git")) {
                on_file_header(line);
            } else if (starts_with(line, "@@")) {
                on_hunk_header(line);
            } else if (in_hunk_) {
                on_hunk_body_line(line);
            }
            if (lines_emitted_ >= max_lines_) {
                hit_global_cap_ = true;
                break;
            }
        }
    }

    std::string finish() && {
        // Per C4 (locked by the audit): per-file flushes BEFORE the
        // global truncation marker so the "+N -M" summary belongs to
        // the file we just emitted, not to content past the cut.
        flush_hunk_truncation();
        flush_file_summary();
        if (hit_global_cap_) {
            emit_line_prefixed("\n", "... (more changes truncated)");
            was_truncated_ = true;
        }
        if (was_truncated_) {
            emit_line("[full diff: mtk git diff --no-compact]");
        }
        return std::move(out_);
    }

private:
    // Append `line` (no internal newlines) as a new logical line in the
    // output. Equivalent to the previous result_.push_back + join_lines:
    // a '\n' separator is inserted between every two emitted lines, no
    // trailing newline.
    void emit_line(std::string_view line) {
        if (lines_emitted_ > 0) out_ += '\n';
        out_.append(line.data(), line.size());
        ++lines_emitted_;
    }
    void emit_line_prefixed(std::string_view prefix, std::string_view line) {
        if (lines_emitted_ > 0) out_ += '\n';
        out_.append(prefix.data(), prefix.size());
        out_.append(line.data(), line.size());
        ++lines_emitted_;
    }

    void on_file_header(std::string_view line) {
        flush_hunk_truncation();
        flush_file_summary();
        current_file_ = extract_b_path(line);
        emit_line_prefixed("\n", current_file_);  // leading \n = blank separator before each file
        added_ = 0;
        removed_ = 0;
        in_hunk_ = false;
        hunk_shown_ = 0;
    }

    void on_hunk_header(std::string_view line) {
        flush_hunk_truncation();
        in_hunk_ = true;
        hunk_shown_ = 0;
        emit_line_prefixed("  ", line);
    }

    void on_hunk_body_line(std::string_view line) {
        if (starts_with(line, "+") && !starts_with(line, "+++")) {
            ++added_;
            emit_or_skip(line);
        } else if (starts_with(line, "-") && !starts_with(line, "---")) {
            ++removed_;
            emit_or_skip(line);
        } else if (hunk_shown_ < max_hunk_lines_ &&
                   !starts_with(line, "\\")) {
            // Context lines are only shown once we've emitted at least
            // one +/- line in this hunk — `hunk_shown_ > 0` is the
            // "we're inside meaningful content" gate.
            if (hunk_shown_ > 0) {
                emit_line_prefixed("  ", line);
                ++hunk_shown_;
            }
        }
    }

    void emit_or_skip(std::string_view line) {
        if (hunk_shown_ < max_hunk_lines_) {
            emit_line_prefixed("  ", line);
            ++hunk_shown_;
        } else {
            ++hunk_skipped_;
        }
    }

    void flush_hunk_truncation() {
        if (hunk_skipped_ > 0) {
            if (lines_emitted_ > 0) out_ += '\n';
            out_ += "  ... (";
            out_ += std::to_string(hunk_skipped_);
            out_ += " lines truncated)";
            ++lines_emitted_;
            was_truncated_ = true;
            hunk_skipped_ = 0;
        }
    }

    void flush_file_summary() {
        if (!current_file_.empty() && (added_ > 0 || removed_ > 0)) {
            if (lines_emitted_ > 0) out_ += '\n';
            out_ += "  +";
            out_ += std::to_string(added_);
            out_ += " -";
            out_ += std::to_string(removed_);
            ++lines_emitted_;
        }
    }

    const std::size_t max_lines_;
    const std::size_t max_hunk_lines_;
    std::string out_;
    std::string current_file_;
    std::size_t lines_emitted_ = 0;
    std::size_t added_ = 0;
    std::size_t removed_ = 0;
    std::size_t hunk_shown_ = 0;
    std::size_t hunk_skipped_ = 0;
    bool in_hunk_ = false;
    bool was_truncated_ = false;
    bool hit_global_cap_ = false;
};

}  // namespace

DiffOptions detect_diff_options(const std::vector<std::string>& args) {
    DiffOptions opts;
    for (const auto& a : args) {
        if (a == "--stat" || a == "--numstat" || a == "--shortstat") opts.wants_stat = true;
        if (a == "--no-compact") opts.wants_no_compact = true;
    }
    return opts;
}

std::string compact_diff(std::string_view diff,
                         std::size_t max_lines,
                         std::size_t max_hunk_lines) {
    DiffCompactor c(max_lines, max_hunk_lines);
    c.feed(diff);
    return std::move(c).finish();
}

}  // namespace internal

namespace {

namespace ex = mtk::core::exec;
using mtk::core::DispatchTokenPtr;

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

}  // namespace

void register_diff(mtk::core::Registry& reg) {
    reg.register_filter(std::make_unique<GitDiffFilter>(),
                        mtk::core::Tier::Builtin, /*is_final=*/true);
}

}  // namespace mtk::cmds::git
