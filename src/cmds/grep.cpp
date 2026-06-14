#include "cmds/grep.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/limits.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::grep {

namespace internal {

namespace {

using mtk::core::utils::to_lower;
using mtk::core::utils::trim_copy;

}  // namespace

std::optional<ParsedMatch> parse_match_line(std::string_view line) {
    if (auto nul = line.find('\0'); nul != std::string_view::npos && nul > 0) {
        std::size_t i = nul + 1;
        std::size_t start_digits = i;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
        if (i > start_digits && i < line.size() && line[i] == ':') {
            ParsedMatch pm;
            pm.file = std::string(line.substr(0, nul));
            try {
                pm.line_num = static_cast<std::size_t>(
                    std::stoull(std::string(line.substr(start_digits, i - start_digits))));
            } catch (...) {
                return std::nullopt;
            }
            pm.content = std::string(line.substr(i + 1));
            return pm;
        }
    }

    std::size_t colon = line.find(':');
    while (colon != std::string_view::npos && colon > 0) {
        std::size_t i = colon + 1;
        std::size_t start_digits = i;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
        if (i > start_digits && i < line.size() && line[i] == ':') {
            ParsedMatch pm;
            pm.file = std::string(line.substr(0, colon));
            try {
                pm.line_num = static_cast<std::size_t>(
                    std::stoull(std::string(line.substr(start_digits, i - start_digits))));
            } catch (...) {
                return std::nullopt;
            }
            pm.content = std::string(line.substr(i + 1));
            return pm;
        }
        colon = line.find(':', colon + 1);
    }
    return std::nullopt;
}

bool has_format_flag(const std::vector<std::string>& extra_args) {
    static const std::vector<std::string> flags = {
        "-c", "--count", "-l", "--files-with-matches",
        "-L", "--files-without-match", "-o", "--only-matching",
        "-Z", "--null",
    };
    for (const auto& a : extra_args) {
        for (const auto& f : flags) {
            if (a == f) return true;
        }
    }
    return false;
}

std::string clean_line(std::string_view raw,
                       std::size_t max_len,
                       std::string_view pattern) {
    std::string trimmed = trim_copy(raw);
    if (trimmed.size() <= max_len) return trimmed;

    std::string lower = to_lower(trimmed);
    std::string plower = to_lower(pattern);
    auto pos = lower.find(plower);
    if (pos == std::string::npos) {
        if (max_len <= 3) return trimmed.substr(0, max_len);
        return trimmed.substr(0, max_len - 3) + "...";
    }

    std::size_t third = max_len / 3;
    std::size_t start = (pos > third) ? pos - third : 0;
    std::size_t end = std::min(start + max_len, trimmed.size());
    if (end == trimmed.size() && trimmed.size() > max_len) {
        start = trimmed.size() - max_len;
    }
    std::string slice = trimmed.substr(start, end - start);
    if (start > 0 && end < trimmed.size()) return "..." + slice + "...";
    if (start > 0) return "..." + slice;
    return slice + "...";
}

std::string compact_path(std::string_view path) {
    namespace plim = mtk::core::limits::grep;
    if (path.size() <= plim::kCompactPathThreshold) return std::string(path);

    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < path.size()) {
        auto j = path.find('/', i);
        if (j == std::string_view::npos) {
            parts.emplace_back(path.substr(i));
            break;
        }
        parts.emplace_back(path.substr(i, j - i));
        i = j + 1;
    }
    if (parts.size() < plim::kCompactPathMinSegments) return std::string(path);

    std::ostringstream o;
    o << parts.front() << "/.../" << parts[parts.size() - 2] << "/" << parts.back();
    return o.str();
}

std::string translate_bre_alternation(std::string_view pattern) {
    std::string out;
    out.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (i + 1 < pattern.size() && pattern[i] == '\\' && pattern[i + 1] == '|') {
            out += '|';
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

}  // namespace internal

namespace {

namespace lim = mtk::core::limits::grep;

struct CliParse {
    std::string pattern;
    std::string path;
    std::vector<std::string> extras;
    bool ok = false;
};

CliParse split_cli(const std::vector<std::string>& args) {
    CliParse r;
    std::vector<std::string> non_flags;
    static const std::vector<std::string> value_flags = {
        "-e", "--regexp", "-f", "--file", "-A", "--after-context",
        "-B", "--before-context", "-C", "--context", "--type", "-t",
        "--glob", "-g", "-m", "--max-count",
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (!a.empty() && a[0] == '-') {
            r.extras.push_back(a);
            for (const auto& vf : value_flags) {
                if (a == vf && i + 1 < args.size()) {
                    r.extras.push_back(args[++i]);
                    break;
                }
            }
        } else {
            non_flags.push_back(a);
        }
    }
    if (non_flags.empty()) return r;
    r.pattern = non_flags.front();
    r.path = non_flags.size() > 1 ? non_flags[1] : ".";
    for (std::size_t k = 2; k < non_flags.size(); ++k) r.extras.push_back(non_flags[k]);
    r.ok = true;
    return r;
}

mtk::core::exec::CapturedOutput run_rg_or_grep(const CliParse& cli) {
    std::string rg_pat = internal::translate_bre_alternation(cli.pattern);
    std::vector<std::string> rg_argv = {"rg", "-n", "-H", "--null", "--no-heading",
                                        "--no-ignore-vcs", rg_pat, cli.path};
    for (const auto& e : cli.extras) {
        if (e == "-r" || e == "--recursive") continue;
        rg_argv.push_back(e);
    }
    auto out = mtk::core::exec::capture(rg_argv);
    if (out.spawned) return out;

    std::vector<std::string> grep_argv = {"grep", "-rnH", cli.pattern, cli.path};
    for (const auto& e : cli.extras) grep_argv.push_back(e);
    return mtk::core::exec::capture(grep_argv);
}

}  // namespace

int run(const std::vector<std::string>& args) {
    auto cli = split_cli(args);
    if (!cli.ok) {
        std::cerr << "mtk grep: missing pattern\n";
        return 2;
    }

    auto captured = run_rg_or_grep(cli);
    if (!captured.spawned) {
        return mtk::core::exit_codes::report_spawn_failure("grep", captured.spawn_error);
    }

    if (internal::has_format_flag(cli.extras)) {
        std::cout << captured.stdout_data;
        if (!captured.stderr_data.empty()) std::cerr << captured.stderr_data;
        return captured.exit_code;
    }

    bool stdout_empty = true;
    for (char c : captured.stdout_data) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            stdout_empty = false;
            break;
        }
    }
    if (stdout_empty) {
        if (captured.exit_code == 2 && !captured.stderr_data.empty()) {
            std::cerr << captured.stderr_data;
        }
        std::cout << "0 matches for '" << cli.pattern << "'\n";
        return captured.exit_code;
    }

    auto raw_lines = mtk::core::utils::split_lines(captured.stdout_data);
    std::size_t total_matches = raw_lines.size();

    std::unordered_map<std::string, std::vector<std::pair<std::size_t, std::string>>> by_file;
    for (const auto& line : raw_lines) {
        auto parsed = internal::parse_match_line(line);
        if (!parsed) continue;
        auto cleaned = internal::clean_line(parsed->content, lim::kMaxLineLen, cli.pattern);
        by_file[parsed->file].emplace_back(parsed->line_num, std::move(cleaned));
    }

    std::ostringstream out;
    out << total_matches << " matches in " << by_file.size() << " files:\n\n";

    std::vector<std::pair<std::string, std::vector<std::pair<std::size_t, std::string>>>> files(
        by_file.begin(), by_file.end());
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::size_t shown = 0;
    for (const auto& [file, matches] : files) {
        if (shown >= lim::kMaxResults) break;
        auto disp = internal::compact_path(file);
        std::size_t emitted_for_file = 0;
        for (const auto& [ln, content] : matches) {
            if (shown >= lim::kMaxResults || emitted_for_file >= lim::kPerFile) break;
            out << disp << ':' << ln << ':' << content << '\n';
            ++shown;
            ++emitted_for_file;
        }
    }

    if (total_matches > shown) {
        out << "[+" << (total_matches - shown) << " more]\n";
    }

    std::cout << out.str();
    return captured.exit_code;
}

}  // namespace mtk::cmds::grep
