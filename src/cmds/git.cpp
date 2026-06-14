#include "cmds/git.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include "core/exec.hpp"
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

}  // namespace internal

namespace {

constexpr const char* kInjectedFormat =
    "--pretty=format:%h %s (%ar) <%an>%n%b%n---END---";

std::vector<std::string> build_git_argv(const std::vector<std::string>& user_args,
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

}  // namespace

int run(const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "log") {
        std::vector<std::string> passthrough = {"git"};
        passthrough.insert(passthrough.end(), args.begin(), args.end());
        return mtk::core::exec::passthrough(passthrough);
    }

    std::vector<std::string> user_args(args.begin() + 1, args.end());
    auto opts = internal::detect_log_options(user_args);
    auto argv = build_git_argv(user_args, opts, /*default_count=*/10);

    auto captured = mtk::core::exec::capture(argv);
    if (!captured.spawned) {
        std::cerr << "mtk: failed to spawn git: " << captured.spawn_error << '\n';
        return 127;
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

}  // namespace mtk::cmds::git
