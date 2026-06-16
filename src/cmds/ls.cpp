#include "cmds/ls.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_map>

#include "core/color.hpp"
#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/filter.hpp"
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

// Correctness critic C12: the previous heuristic operated on the raw line
// and matched any trailing space-dot, silently dropping files literally
// named "foo .". Fixed by checking the extracted filename only.
bool is_dotdir(std::string_view filename) noexcept {
    return filename == "." || filename == "..";
}

std::optional<LsEntry> parse_ls_line(const std::string& line) {
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

namespace {

// Internal buckets used during compaction. Local to compact_ls — pulled
// out so the loop body and the two formatters can share the same data.
struct DirEntry {
    std::string name;
    std::optional<std::string> octal;
};
struct FileEntry {
    std::string name;
    std::string size;
    std::optional<std::string> octal;
};
struct Buckets {
    std::vector<DirEntry> dirs;
    std::vector<FileEntry> files;
    std::unordered_map<std::string, std::size_t> by_ext;
    std::size_t lines_seen = 0;
    std::size_t dotdirs = 0;
    std::size_t parsed_count = 0;
};

bool is_noise_dir(const std::string& name) {
    for (const auto& n : noise_dirs()) {
        if (name == n) return true;
    }
    return false;
}

std::string extension_of(const std::string& name) {
    auto dot = name.rfind('.');
    return (dot == std::string::npos) ? "no ext" : name.substr(dot);
}

Buckets bucket_lines(std::string_view raw, bool show_all, bool show_long) {
    Buckets b;
    for (const auto& line : mtk::core::utils::split_lines_view(raw)) {
        if (starts_with(line, "total ") || line.empty()) continue;
        ++b.lines_seen;
        auto parsed = parse_ls_line(std::string(line));
        if (!parsed) continue;  // unparseable; lines_seen - dotdirs - parsed_count counts these
        if (is_dotdir(parsed->name)) { ++b.dotdirs; continue; }
        ++b.parsed_count;

        if (!show_all && is_noise_dir(parsed->name)) continue;

        std::optional<std::string> octal;
        if (show_long) octal = perms_to_octal(parsed->perms);

        if (parsed->type == 'd') {
            b.dirs.push_back(DirEntry{parsed->name, std::move(octal)});
        } else {
            ++b.by_ext[extension_of(parsed->name)];
            b.files.push_back(FileEntry{parsed->name, human_size(parsed->size),
                                        std::move(octal)});
        }
    }
    return b;
}

std::string format_entries(const Buckets& b) {
    std::ostringstream entries;
    for (const auto& d : b.dirs) {
        // octal perms (long-mode) dim. Directory names + trailing /
        // in blue — classic Unix `ls` convention.
        if (d.octal) entries << mtk::core::color::dim(*d.octal) << "  ";
        entries << mtk::core::color::blue(d.name + "/") << '\n';
    }
    for (const auto& f : b.files) {
        // File names plain (their colors would conflict with extension
        // semantics that vary too much); size in dim so the name reads
        // first.
        if (f.octal) entries << mtk::core::color::dim(*f.octal) << "  ";
        entries << f.name << "  " << mtk::core::color::dim(f.size) << '\n';
    }
    return entries.str();
}

std::string format_summary(const Buckets& b) {
    // Build the summary as a single string and dim it as a unit — the
    // line is meta-information about the listing, not part of it.
    std::ostringstream summary;
    summary << "Summary: " << b.files.size() << " files, " << b.dirs.size() << " dirs";
    if (!b.by_ext.empty()) {
        std::vector<std::pair<std::string, std::size_t>> ext_counts(
            b.by_ext.begin(), b.by_ext.end());
        std::sort(ext_counts.begin(), ext_counts.end(),
                  [](const auto& a, const auto& c) { return a.second > c.second; });
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
    return "\n" + mtk::core::color::dim(summary.str()) + "\n";
}

}  // namespace

CompactResult compact_ls(std::string_view raw, bool show_all, bool show_long) {
    auto b = bucket_lines(raw, show_all, show_long);

    CompactResult r;
    r.parsed_count = b.parsed_count;
    r.has_unparseable = b.lines_seen > b.dotdirs + b.parsed_count;

    if (b.dirs.empty() && b.files.empty()) {
        // Three "(empty)" cases (post-audit, restoring the all-noise
        // branch that the Phase 4 decomposition dropped):
        //   - truly empty: subprocess returned nothing.
        //   - only-dotdirs: only `.` and `..` were present.
        //   - all-noise: parsed real entries but every one was a noise
        //     dir (`node_modules`, `.git`, etc.) filtered out when
        //     show_all is false. User asked for ls, got nothing visible
        //     — say "(empty)" rather than emitting blank stdout.
        // If we saw lines but couldn't parse them AND they weren't
        // dotdirs/noise, we lost real content — bail without claiming.
        const bool truly_empty = b.lines_seen == 0;
        const bool only_dotdirs = b.parsed_count == 0 && b.dotdirs == b.lines_seen;
        const bool all_noise = b.parsed_count > 0;
        if (truly_empty || only_dotdirs || all_noise) {
            r.entries = mtk::core::color::dim("(empty)") + "\n";
        }
        return r;
    }

    r.entries = format_entries(b);
    r.summary = format_summary(b);
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

// Per A1: dedicated C++ filter for `ls`. Reuses the pure internal helpers
// (parse_args, compact_ls) — only the dispatch shell changed.
class LsFilter final : public mtk::core::Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "ls"; }
    [[nodiscard]] std::string_view source() const noexcept override { return "builtin"; }
    [[nodiscard]] std::string_view literal_first_token() const noexcept override { return "ls"; }

    [[nodiscard]] std::optional<mtk::core::DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override {
        if (argv.empty() || argv[0] != "ls") return std::nullopt;
        return mtk::core::DispatchTokenPtr{};
    }

    [[nodiscard]] mtk::core::exec::ExecOutcome
    run(mtk::core::DispatchTokenPtr,
        const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override {
        std::vector<std::string> user_args(argv.begin() + 1, argv.end());
        auto opts = internal::parse_args(user_args);
        auto cmd_argv = build_ls_argv(user_args);

        auto outcome = ctx.capture(cmd_argv, {{"LC_ALL", "C"}});
        const auto* ran = ctx.as_ran(outcome);
        if (!ran) return outcome;  // SpawnFailed flows through

        if (!ran->clean()) {
            return outcome;  // pass raw on non-zero
        }

        internal::CompactResult result;
        try {
            result = internal::compact_ls(ran->stdout_data, opts.show_all, opts.show_long);
        } catch (const std::exception&) {
            return outcome;  // fallback to raw per A4
        }

        // Per perf critic P1 (Round F): bucket_lines already saw every
        // line; reusing its has_unparseable flag avoids a second
        // split_lines + parse_ls_line regex walk over the same input.
        if (result.parsed_count == 0 && result.has_unparseable) {
            return outcome;  // parser found nothing real — pass raw
        }

        return mtk::core::exec::Ran{
            result.entries + result.summary,
            std::string{},
            0,
            ran->truncated, ran->timed_out, ran->killed_by_signal,
        };
    }
};

void register_builtins(mtk::core::Registry& reg) {
    reg.register_filter(std::make_unique<LsFilter>(),
                        mtk::core::Tier::Builtin, /*is_final=*/true);
}

}  // namespace mtk::cmds::ls
