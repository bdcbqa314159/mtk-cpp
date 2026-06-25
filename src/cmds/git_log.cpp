#include "cmds/git.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/constants.hpp"
#include "core/exec.hpp"
#include "core/filter.hpp"
#include "core/limits.hpp"
#include "core/run_context.hpp"
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

}  // namespace internal

namespace {

namespace ex = mtk::core::exec;
using mtk::core::DispatchTokenPtr;

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

}  // namespace

void register_log(mtk::core::Registry& reg) {
    reg.register_filter(std::make_unique<GitLogFilter>(),
                        mtk::core::Tier::Builtin, /*is_final=*/true);
}

}  // namespace mtk::cmds::git
