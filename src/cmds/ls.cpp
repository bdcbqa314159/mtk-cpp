#include "cmds/ls.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_map>

#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/limits.hpp"
#include "core/run_context.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::ls {

namespace internal {

namespace {

const std::vector<std::string>& noise_dirs() {
    static const std::vector<std::string> v = {
        "node_modules", ".git", "target", "__pycache__", ".next",
        "dist", "build", ".cache", ".turbo", ".vercel",
        ".pytest_cache", ".mypy_cache", ".tox", ".venv", "venv",
        "env", "coverage", ".nyc_output", ".DS_Store", "Thumbs.db",
        ".idea", ".vscode", ".vs", ".eggs",
    };
    return v;
}

const std::regex& ls_date_regex() {
    static const std::regex re(
        R"(\s+(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\s+\d{1,2}\s+(?:\d{4}|\d{2}:\d{2})\s+)");
    return re;
}

using mtk::core::utils::starts_with;
using mtk::core::utils::trim_copy;

std::vector<std::string> split_whitespace(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        std::size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) parts.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return parts;
}

}  // namespace

LsOptions parse_args(const std::vector<std::string>& args) {
    LsOptions opts;
    for (const auto& a : args) {
        if (a == "--all") {
            opts.show_all = true;
            continue;
        }
        if (a == "--full-time" || a == "--format=long" || a == "--format=verbose") {
            opts.show_long = true;
            continue;
        }
        if (a.size() >= 2 && a[0] == '-' && a[1] != '-') {
            for (char c : a.substr(1)) {
                if (c == 'a') opts.show_all = true;
                if (c == 'l' || c == 'g' || c == 'n' || c == 'o') opts.show_long = true;
            }
        }
    }
    return opts;
}

bool is_dotdir(std::string_view line) {
    std::string t = trim_copy(line);
    return !t.empty() && (t.back() == '.' &&
                          (t.size() == 1 || t[t.size() - 2] == '.' ||
                           std::isspace(static_cast<unsigned char>(t[t.size() - 2]))));
}

std::optional<LsEntry> parse_ls_line(const std::string& line) {
    if (is_dotdir(line)) return std::nullopt;

    std::smatch m;
    if (!std::regex_search(line, m, ls_date_regex())) return std::nullopt;

    auto date_start = static_cast<std::size_t>(m.position(0));
    auto date_end = date_start + static_cast<std::size_t>(m.length(0));

    LsEntry e;
    e.name = line.substr(date_end);

    std::string before_date = line.substr(0, date_start);
    auto parts = split_whitespace(before_date);
    if (parts.size() < 4) return std::nullopt;

    e.perms = parts[0];
    if (e.perms.empty()) return std::nullopt;
    e.type = e.perms.front();

    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        try {
            std::size_t pos = 0;
            std::uint64_t s = std::stoull(*it, &pos);
            if (pos == it->size()) {
                e.size = s;
                break;
            }
        } catch (...) {
        }
    }
    return e;
}

std::optional<std::string> perms_to_octal(const std::string& perms) {
    if (perms.size() < 10) return std::nullopt;
    for (char c : perms) {
        if (static_cast<unsigned char>(c) > 127) return std::nullopt;
    }
    auto perm_value = [](bool r, bool w, bool x) -> unsigned {
        return (unsigned{r} << 2u) | (unsigned{w} << 1u) | unsigned{x};
    };
    bool ox = perms[3] == 'x' || perms[3] == 's';
    bool gx = perms[6] == 'x' || perms[6] == 's';
    bool tx = perms[9] == 'x' || perms[9] == 't';
    unsigned owner = perm_value(perms[1] == 'r', perms[2] == 'w', ox);
    unsigned group = perm_value(perms[4] == 'r', perms[5] == 'w', gx);
    unsigned other = perm_value(perms[7] == 'r', perms[8] == 'w', tx);
    bool setuid = perms[3] == 's' || perms[3] == 'S';
    bool setgid = perms[6] == 's' || perms[6] == 'S';
    bool sticky = perms[9] == 't' || perms[9] == 'T';
    unsigned special = perm_value(setuid, setgid, sticky);
    char buf[8];
    if (special > 0) {
        std::snprintf(buf, sizeof(buf), "%u%u%u%u", special, owner, group, other);
    } else {
        std::snprintf(buf, sizeof(buf), "%u%u%u", owner, group, other);
    }
    return std::string(buf);
}

std::string human_size(std::uint64_t bytes) {
    namespace flim = mtk::core::limits::fmt;
    char buf[32];
    if (bytes >= flim::kHumanMB) {
        std::snprintf(buf, sizeof(buf), "%.1fM",
                      static_cast<double>(bytes) / static_cast<double>(flim::kHumanMB));
    } else if (bytes >= flim::kHumanKB) {
        std::snprintf(buf, sizeof(buf), "%.1fK",
                      static_cast<double>(bytes) / static_cast<double>(flim::kHumanKB));
    } else {
        std::snprintf(buf, sizeof(buf), "%lluB", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

CompactResult compact_ls(std::string_view raw, bool show_all, bool show_long) {
    CompactResult r;
    std::vector<std::pair<std::string, std::optional<std::string>>> dirs;
    std::vector<std::tuple<std::string, std::string, std::optional<std::string>>> files;
    std::unordered_map<std::string, std::size_t> by_ext;
    std::size_t lines_seen = 0;
    std::size_t dotdirs = 0;

    for (const auto& line : mtk::core::utils::split_lines(raw)) {
        if (starts_with(line, "total ") || line.empty()) continue;
        ++lines_seen;
        auto parsed = parse_ls_line(line);
        if (!parsed) {
            if (is_dotdir(line)) ++dotdirs;
            continue;
        }
        ++r.parsed_count;

        if (!show_all) {
            bool noise = false;
            for (const auto& n : noise_dirs()) {
                if (parsed->name == n) {
                    noise = true;
                    break;
                }
            }
            if (noise) continue;
        }

        std::optional<std::string> octal;
        if (show_long) octal = perms_to_octal(parsed->perms);

        if (parsed->type == 'd') {
            dirs.emplace_back(parsed->name, std::move(octal));
        } else {
            std::string ext;
            auto dot = parsed->name.rfind('.');
            ext = (dot == std::string::npos) ? "no ext" : parsed->name.substr(dot);
            ++by_ext[ext];
            files.emplace_back(parsed->name, human_size(parsed->size), std::move(octal));
        }
    }

    if (dirs.empty() && files.empty()) {
        if (lines_seen > 0 && r.parsed_count == 0) {
            if (dotdirs == lines_seen) {
                r.entries = "(empty)\n";
            }
            return r;
        }
        r.entries = "(empty)\n";
        return r;
    }

    std::ostringstream entries;
    for (const auto& [name, octal] : dirs) {
        if (octal) entries << *octal << "  ";
        entries << name << "/\n";
    }
    for (const auto& [name, sz, octal] : files) {
        if (octal) entries << *octal << "  ";
        entries << name << "  " << sz << '\n';
    }
    r.entries = entries.str();

    std::ostringstream summary;
    summary << "\nSummary: " << files.size() << " files, " << dirs.size() << " dirs";
    if (!by_ext.empty()) {
        std::vector<std::pair<std::string, std::size_t>> ext_counts(by_ext.begin(), by_ext.end());
        std::sort(ext_counts.begin(), ext_counts.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        constexpr auto kMaxExt = mtk::core::limits::ls::kExtSummaryMaxCount;
        summary << " (";
        for (std::size_t i = 0; i < std::min(kMaxExt, ext_counts.size()); ++i) {
            if (i) summary << ", ";
            summary << ext_counts[i].second << ' ' << ext_counts[i].first;
        }
        if (ext_counts.size() > kMaxExt) {
            summary << ", +" << (ext_counts.size() - kMaxExt) << " more";
        }
        summary << ')';
    }
    summary << '\n';
    r.summary = summary.str();
    return r;
}

}  // namespace internal

namespace {

std::vector<std::string> build_ls_argv(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"ls", "-la"};
    std::vector<std::string> paths;
    for (const auto& a : args) {
        if (a.empty()) continue;
        if (a == "--all") continue;
        if (a[0] == '-') {
            if (a.size() >= 2 && a[1] == '-') {
                argv.push_back(a);
                continue;
            }
            std::string extra;
            for (char c : a.substr(1)) {
                if (c != 'l' && c != 'a' && c != 'h') extra += c;
            }
            if (!extra.empty()) argv.push_back("-" + extra);
        } else {
            paths.push_back(a);
        }
    }
    if (paths.empty()) {
        argv.emplace_back(".");
    } else {
        for (const auto& p : paths) argv.push_back(p);
    }
    return argv;
}

}  // namespace

int run(const std::vector<std::string>& args) {
    auto opts = internal::parse_args(args);
    auto argv = build_ls_argv(args);

    mtk::core::RunContext ctx;
    auto outcome = ctx.capture(argv, {{"LC_ALL", "C"}});
    const auto* ran = ctx.as_ran(outcome);
    if (!ran) return ctx.report_spawn_failure(outcome, "ls");

    if (!ran->clean()) {
        std::cout << ran->stdout_data;
        if (!ran->stderr_data.empty()) std::cerr << ran->stderr_data;
        return ran->exit_code;
    }

    internal::CompactResult result;
    try {
        result = internal::compact_ls(ran->stdout_data, opts.show_all, opts.show_long);
    } catch (const std::exception& e) {
        std::cerr << "mtk ls: filter warning: " << e.what() << '\n';
        std::cout << ran->stdout_data;
        return 0;
    }

    bool has_real_content = false;
    for (const auto& line : mtk::core::utils::split_lines(ran->stdout_data)) {
        if (line.empty()) continue;
        if (line.substr(0, 6) == "total ") continue;
        if (internal::is_dotdir(line)) continue;
        has_real_content = true;
        break;
    }
    if (result.parsed_count == 0 && has_real_content) {
        std::cout << ran->stdout_data;
        return 0;
    }

    std::cout << result.entries << result.summary;
    return 0;
}

}  // namespace mtk::cmds::ls
