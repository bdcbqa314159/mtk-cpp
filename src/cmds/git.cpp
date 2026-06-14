#include "cmds/git.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/tee.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::git {

namespace internal {

namespace {

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
}

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
    static const std::string sep = "---END---";

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

        auto lines = mtk::core::utils::split_lines(chunk);
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

std::string trim_copy(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return std::string(s.substr(i, j - i));
}

}  // namespace

std::optional<std::string> extract_state_header(std::string_view plain) {
    for (const auto& raw_line : mtk::core::utils::split_lines(plain)) {
        std::string stripped = trim_copy(raw_line);
        for (const auto& stopper : state_stoppers()) {
            if (starts_with(stripped, stopper)) return std::nullopt;
        }
        if (auto state = detect_status_state(stripped)) return state;
    }
    return std::nullopt;
}

std::optional<std::string> extract_detached_head(std::string_view plain) {
    for (const auto& raw_line : mtk::core::utils::split_lines(plain)) {
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

std::string extract_b_path(const std::string& diff_git_line) {
    auto pos = diff_git_line.find(" b/");
    if (pos == std::string::npos) return "unknown";
    return diff_git_line.substr(pos + 3);
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

    for (const auto& line : mtk::core::utils::split_lines(diff)) {
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
            result.push_back("  " + line);
        } else if (in_hunk) {
            if (starts_with(line, "+") && !starts_with(line, "+++")) {
                ++added;
                if (hunk_shown < max_hunk_lines) {
                    result.push_back("  " + line);
                    ++hunk_shown;
                } else {
                    ++hunk_skipped;
                }
            } else if (starts_with(line, "-") && !starts_with(line, "---")) {
                ++removed;
                if (hunk_shown < max_hunk_lines) {
                    result.push_back("  " + line);
                    ++hunk_shown;
                } else {
                    ++hunk_skipped;
                }
            } else if (hunk_shown < max_hunk_lines && !starts_with(line, "\\")) {
                if (hunk_shown > 0) {
                    result.push_back("  " + line);
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

constexpr const char* kInjectedFormat =
    "--pretty=format:%h %s (%ar) <%an>%n%b%n---END---";

std::vector<std::string> build_log_argv(const std::vector<std::string>& user_args,
                                        const internal::LogOptions& opts,
                                        std::size_t default_count) {
    std::vector<std::string> argv = {"git", "log"};
    if (!opts.user_set_count) {
        argv.push_back("-" + std::to_string(default_count));
    }
    if (!opts.user_set_format) {
        argv.push_back(kInjectedFormat);
    }
    for (const auto& a : user_args) argv.push_back(a);
    return argv;
}

int run_log(const std::vector<std::string>& user_args) {
    auto opts = internal::detect_log_options(user_args);
    auto argv = build_log_argv(user_args, opts, /*default_count=*/10);

    auto captured = mtk::core::exec::capture(argv);
    if (!captured.spawned) {
        return mtk::core::exit_codes::report_spawn_failure("git", captured.spawn_error);
    }

    if (opts.user_set_format) {
        std::cout << captured.stdout_data;
    } else {
        std::string filtered;
        try {
            filtered = internal::filter_log_output(
                captured.stdout_data, /*max_commits=*/opts.user_set_count ? 1000 : 10,
                /*header_width=*/opts.user_set_count ? 120 : 80,
                /*max_body_lines=*/3);
        } catch (const std::exception& e) {
            std::cerr << "mtk: filter warning: " << e.what() << '\n';
            filtered = captured.stdout_data;
        }
        std::cout << filtered;
        if (!filtered.empty() && filtered.back() != '\n') std::cout << '\n';
    }

    if (captured.exit_code != 0) {
        if (auto hint = mtk::core::tee::tee_and_hint(captured.stdout_data, "git_log",
                                                    captured.exit_code)) {
            std::cerr << *hint << '\n';
        }
        if (!captured.stderr_data.empty()) std::cerr << captured.stderr_data;
    }
    return captured.exit_code;
}

bool is_blob_show_arg(const std::string& a) {
    return !a.empty() && a[0] != '-' && a.find(':') != std::string::npos;
}

int run_show(const std::vector<std::string>& user_args) {
    bool wants_stat_only = false;
    bool wants_format = false;
    bool wants_blob = false;
    for (const auto& a : user_args) {
        if (a == "--stat" || a == "--numstat" || a == "--shortstat") wants_stat_only = true;
        if (a.rfind("--pretty", 0) == 0 || a.rfind("--format", 0) == 0) wants_format = true;
        if (is_blob_show_arg(a)) wants_blob = true;
    }

    if (wants_stat_only || wants_format || wants_blob) {
        std::vector<std::string> argv = {"git", "show"};
        argv.insert(argv.end(), user_args.begin(), user_args.end());
        auto captured = mtk::core::exec::capture(argv);
        if (!captured.spawned) {
            std::cerr << "mtk: failed to spawn git: " << captured.spawn_error << '\n';
            return 127;
        }
        std::cout << captured.stdout_data;
        if (!captured.stderr_data.empty()) std::cerr << captured.stderr_data;
        return captured.exit_code;
    }

    std::vector<std::string> sum_argv = {"git", "show", "--no-patch",
                                         "--pretty=format:%h %s (%ar) <%an>"};
    sum_argv.insert(sum_argv.end(), user_args.begin(), user_args.end());
    auto sum = mtk::core::exec::capture(sum_argv);
    if (!sum.spawned) {
        return mtk::core::exit_codes::report_spawn_failure("git", sum.spawn_error);
    }
    if (sum.exit_code != 0) {
        if (!sum.stderr_data.empty()) std::cerr << sum.stderr_data;
        return sum.exit_code;
    }
    std::cout << sum.stdout_data;
    if (!sum.stdout_data.empty() && sum.stdout_data.back() != '\n') std::cout << '\n';

    std::vector<std::string> stat_argv = {"git", "show", "--stat", "--pretty=format:"};
    stat_argv.insert(stat_argv.end(), user_args.begin(), user_args.end());
    auto stat = mtk::core::exec::capture(stat_argv);
    if (!stat.stdout_data.empty()) std::cout << stat.stdout_data;

    std::vector<std::string> diff_argv = {"git", "show", "--pretty=format:"};
    diff_argv.insert(diff_argv.end(), user_args.begin(), user_args.end());
    auto diff = mtk::core::exec::capture(diff_argv);
    if (!diff.stdout_data.empty()) {
        std::string compacted;
        try {
            compacted = internal::compact_diff(diff.stdout_data);
        } catch (const std::exception& e) {
            std::cerr << "mtk: filter warning: " << e.what() << '\n';
            compacted = diff.stdout_data;
        }
        std::cout << compacted;
        if (!compacted.empty() && compacted.back() != '\n') std::cout << '\n';
    }
    return 0;
}

int run_diff(const std::vector<std::string>& user_args) {
    auto opts = internal::detect_diff_options(user_args);

    std::vector<std::string> args_no_compact;
    args_no_compact.reserve(user_args.size());
    for (const auto& a : user_args) {
        if (a != "--no-compact") args_no_compact.push_back(a);
    }

    if (opts.wants_stat || opts.wants_no_compact) {
        std::vector<std::string> argv = {"git", "diff"};
        argv.insert(argv.end(), args_no_compact.begin(), args_no_compact.end());
        auto captured = mtk::core::exec::capture(argv);
        if (!captured.spawned) {
            std::cerr << "mtk: failed to spawn git: " << captured.spawn_error << '\n';
            return 127;
        }
        std::cout << captured.stdout_data;
        if (!captured.stderr_data.empty()) std::cerr << captured.stderr_data;
        return captured.exit_code;
    }

    std::vector<std::string> stat_argv = {"git", "diff", "--stat"};
    stat_argv.insert(stat_argv.end(), args_no_compact.begin(), args_no_compact.end());
    auto stat = mtk::core::exec::capture(stat_argv);
    if (!stat.spawned) {
        return mtk::core::exit_codes::report_spawn_failure("git", stat.spawn_error);
    }
    if (stat.exit_code != 0) {
        std::cout << stat.stdout_data;
        if (!stat.stderr_data.empty()) std::cerr << stat.stderr_data;
        return stat.exit_code;
    }

    if (!stat.stdout_data.empty()) {
        std::cout << stat.stdout_data;
        if (stat.stdout_data.back() != '\n') std::cout << '\n';
    }

    std::vector<std::string> diff_argv = {"git", "diff"};
    diff_argv.insert(diff_argv.end(), args_no_compact.begin(), args_no_compact.end());
    auto diff = mtk::core::exec::capture(diff_argv);
    if (!diff.spawned) {
        return mtk::core::exit_codes::report_spawn_failure("git", diff.spawn_error);
    }

    if (!diff.stdout_data.empty()) {
        std::cout << "\nChanges:";
        std::string compacted;
        try {
            compacted = internal::compact_diff(diff.stdout_data);
        } catch (const std::exception& e) {
            std::cerr << "mtk: filter warning: " << e.what() << '\n';
            compacted = diff.stdout_data;
        }
        std::cout << compacted;
        if (!compacted.empty() && compacted.back() != '\n') std::cout << '\n';
    }

    return diff.exit_code;
}

int run_status(const std::vector<std::string>& user_args) {
    if (!internal::uses_compact_status_path(user_args)) {
        std::vector<std::string> argv = {"git", "status"};
        argv.insert(argv.end(), user_args.begin(), user_args.end());
        auto captured = mtk::core::exec::capture(argv);
        if (!captured.spawned) {
            std::cerr << "mtk: failed to spawn git: " << captured.spawn_error << '\n';
            return 127;
        }
        if (captured.exit_code != 0) {
            std::cout << captured.stdout_data;
            if (!captured.stderr_data.empty()) std::cerr << captured.stderr_data;
            return captured.exit_code;
        }
        std::string filtered;
        try {
            filtered = internal::filter_status_with_args(captured.stdout_data);
        } catch (const std::exception& e) {
            std::cerr << "mtk: filter warning: " << e.what() << '\n';
            filtered = captured.stdout_data;
        }
        std::cout << filtered;
        if (!filtered.empty() && filtered.back() != '\n') std::cout << '\n';
        return 0;
    }

    std::vector<std::string> plain_argv = {"git", "status"};
    plain_argv.insert(plain_argv.end(), user_args.begin(), user_args.end());
    auto plain = mtk::core::exec::capture(plain_argv);

    std::vector<std::string> porcelain_argv = {"git", "status", "--porcelain", "-b"};
    auto porcelain = mtk::core::exec::capture(porcelain_argv);
    if (!porcelain.spawned) {
        return mtk::core::exit_codes::report_spawn_failure("git", porcelain.spawn_error);
    }

    if (!porcelain.stderr_data.empty() &&
        porcelain.stderr_data.find("not a git repository") != std::string::npos) {
        std::cerr << "Not a git repository\n";
        return porcelain.exit_code != 0 ? porcelain.exit_code : 128;
    }

    // Plain status is only used to surface state/detached-HEAD info that
    // porcelain collapses. If plain failed to spawn or returned non-zero,
    // skip those extractions rather than feeding garbage to the parsers.
    const bool plain_usable = plain.spawned && plain.exit_code == 0;
    std::string_view plain_view = plain_usable ? plain.stdout_data : std::string_view{};

    auto detached = internal::extract_detached_head(plain_view);
    auto formatted = internal::format_status_output(porcelain.stdout_data, detached);
    auto state = internal::extract_state_header(plain_view);

    if (state) std::cout << *state << '\n';
    std::cout << formatted;
    if (!formatted.empty() && formatted.back() != '\n') std::cout << '\n';

    return porcelain.exit_code;
}

}  // namespace

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        return mtk::core::exec::passthrough({"git"});
    }
    const std::string& sub = args[0];
    std::vector<std::string> user_args(args.begin() + 1, args.end());

    if (sub == "log") return run_log(user_args);
    if (sub == "status") return run_status(user_args);
    if (sub == "diff") return run_diff(user_args);
    if (sub == "show") return run_show(user_args);

    std::vector<std::string> passthrough = {"git"};
    passthrough.insert(passthrough.end(), args.begin(), args.end());
    return mtk::core::exec::passthrough(passthrough);
}

}  // namespace mtk::cmds::git
