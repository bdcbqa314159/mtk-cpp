#include "cmds/git.hpp"

#include <string>

#include "core/exec.hpp"
#include "core/filter.hpp"
#include "core/run_context.hpp"

namespace mtk::cmds::git {

namespace {

namespace ex = mtk::core::exec;
using mtk::core::DispatchTokenPtr;

bool is_blob_show_arg(const std::string& a) {
    return !a.empty() && a[0] != '-' && a.find(':') != std::string::npos;
}

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

        std::string_view full(show->stdout_data);

        // Per audit: `git show A B [...]` is legal and emits one
        // `\n---\n` per commit. The compact path assumes a single
        // commit and would garble multi-commit output (the second
        // commit's summary/stat would be parsed as if it were diff
        // content). Count separators; >1 means multi-commit — emit the
        // captured output verbatim (it's already reasonably compressed
        // via our `--pretty=format:%h %s (%ar) <%an>` injection; we
        // just skip compact_diff because it isn't commit-boundary-
        // aware).
        std::size_t sep_count = 0;
        for (std::size_t pos = full.find("\n---\n");
             pos != std::string_view::npos;
             pos = full.find("\n---\n", pos + 1)) {
            ++sep_count;
        }
        if (sep_count > 1) {
            return ex::Ran{std::string(full), std::string{}, 0,
                           show->truncated, show->timed_out,
                           show->killed_by_signal};
        }

        // Split at the first `diff --git` line: everything before is the
        // summary line + --stat block; everything from there is the patch.
        auto patch_start = full.find("diff --git");

        std::string out;
        // Git inserts a literal `---\n` commit-boundary separator between
        // the pretty-format prefix and the --stat block. Strip it once
        // (we've already verified single-commit above).
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

void register_show(mtk::core::Registry& reg) {
    reg.register_filter(std::make_unique<GitShowFilter>(),
                        mtk::core::Tier::Builtin, /*is_final=*/true);
}

}  // namespace mtk::cmds::git
