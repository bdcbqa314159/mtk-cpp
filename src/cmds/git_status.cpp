#include "cmds/git.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/exec.hpp"
#include "core/filter.hpp"
#include "core/run_context.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::git {

namespace internal {

namespace {

using mtk::core::utils::starts_with;
using mtk::core::utils::trim_copy;

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

}  // namespace

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
        out.emplace_back("clean -- nothing to commit");
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

}  // namespace internal

namespace {

namespace ex = mtk::core::exec;
using mtk::core::DispatchTokenPtr;

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

}  // namespace

void register_status(mtk::core::Registry& reg) {
    reg.register_filter(std::make_unique<GitStatusFilter>(),
                        mtk::core::Tier::Builtin, /*is_final=*/true);
}

}  // namespace mtk::cmds::git
