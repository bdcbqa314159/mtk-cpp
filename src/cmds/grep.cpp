#include "cmds/grep.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "core/color.hpp"
#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/filter.hpp"
#include "core/limits.hpp"
#include "core/run_context.hpp"
#include "core/utils.hpp"

namespace mtk::cmds::grep {

namespace internal {

namespace {

using mtk::core::utils::to_lower;
using mtk::core::utils::trim_copy;

}  // namespace

namespace {

// Try parsing `<file><sep><digits>:<content>` given a candidate position
// for the file/line separator. Returns nullopt if the digits-then-colon
// shape doesn't follow `sep_pos`. Shared between the NUL-separated and
// colon-separated parse paths.
std::optional<ParsedMatch> try_parse_at(std::string_view line, std::size_t sep_pos) {
    std::size_t start_digits = sep_pos + 1;
    std::size_t i = start_digits;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
    if (i == start_digits || i >= line.size() || line[i] != ':') {
        return std::nullopt;
    }
    ParsedMatch pm;
    pm.file = std::string(line.substr(0, sep_pos));
    try {
        pm.line_num = static_cast<std::size_t>(
            std::stoull(std::string(line.substr(start_digits, i - start_digits))));
    } catch (...) {
        return std::nullopt;
    }
    pm.content = std::string(line.substr(i + 1));
    return pm;
}

}  // namespace

// Accepts both formats real-world rg/grep emit:
//   - NUL-separated (rg -Z, grep --null): `file\0line:content`
//   - Colon-separated:                    `file:line:content`
// In the colon case, filenames can themselves contain colons, so we
// scan colon-by-colon until the digits-then-colon shape lines up.
//
// Per audit: when NUL is present but malformed, return nullopt rather
// than falling through to the colon scan. The colon scan could find a
// colon BEFORE the NUL and synthesise a confidently-wrong attribution
// (e.g. `weird:42:rest\0broken:7:more` was parsing as file="weird",
// line=42). No real-world rg/grep emits malformed-NUL output — the
// fallthrough was speculative generality.
std::optional<ParsedMatch> parse_match_line(std::string_view line) {
    if (auto nul = line.find('\0'); nul != std::string_view::npos && nul > 0) {
        return try_parse_at(line, nul);
    }

    for (std::size_t colon = line.find(':');
         colon != std::string_view::npos && colon > 0;
         colon = line.find(':', colon + 1)) {
        if (auto pm = try_parse_at(line, colon)) return pm;
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

std::vector<std::string> build_rg_argv(const CliParse& cli) {
    std::string rg_pat = internal::translate_bre_alternation(cli.pattern);
    std::vector<std::string> argv = {"rg", "-n", "-H", "--null", "--no-heading",
                                     "--no-ignore-vcs", rg_pat, cli.path};
    for (const auto& e : cli.extras) {
        if (e == "-r" || e == "--recursive") continue;
        argv.push_back(e);
    }
    return argv;
}

std::vector<std::string> build_grep_argv(const CliParse& cli) {
    // -Z forces NUL-separated `file\0line:content` output. Without it,
    // grep emits `file:line:content` and filenames containing `:digits:`
    // (Windows drive letters, oddly-named files) make parse_match_line's
    // colon scan attribute matches to the wrong file. GNU and BSD greps
    // both honour -Z.
    std::vector<std::string> argv = {"grep", "-rnHZ", cli.pattern, cli.path};
    for (const auto& e : cli.extras) argv.push_back(e);
    return argv;
}

}  // namespace

class GrepFilter final : public mtk::core::Filter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "grep"; }
    [[nodiscard]] std::string_view source() const noexcept override { return "builtin"; }

    [[nodiscard]] std::optional<mtk::core::DispatchTokenPtr>
    try_match(const std::vector<std::string>& argv) const noexcept override {
        if (argv.empty()) return std::nullopt;
        if (argv[0] != "grep" && argv[0] != "rg") return std::nullopt;
        return mtk::core::DispatchTokenPtr{};
    }

    [[nodiscard]] mtk::core::exec::ExecOutcome
    run(mtk::core::DispatchTokenPtr,
        const std::vector<std::string>& argv,
        mtk::core::RunContext& ctx) override {
        std::vector<std::string> user_args(argv.begin() + 1, argv.end());
        auto cli = split_cli(user_args);
        if (!cli.ok) {
            return mtk::core::exec::Ran{
                std::string{}, "mtk grep: missing pattern\n",
                mtk::core::exit_codes::kUsage, false, false, 0,
            };
        }

        auto outcome = ctx.capture(build_rg_argv(cli));
        if (ctx.is_spawn_failed(outcome)) {
            outcome = ctx.capture(build_grep_argv(cli));
        }
        const auto* ran = ctx.as_ran(outcome);
        if (!ran) return outcome;

        if (internal::has_format_flag(cli.extras)) {
            return outcome;  // passthrough
        }

        bool stdout_empty = true;
        for (char c : ran->stdout_data) {
            if (!std::isspace(static_cast<unsigned char>(c))) { stdout_empty = false; break; }
        }
        if (stdout_empty) {
            return mtk::core::exec::Ran{
                mtk::core::color::dim("0 matches for '" + cli.pattern + "'") + "\n",
                (ran->exit_code == 2) ? ran->stderr_data : std::string{},
                ran->exit_code,
                ran->truncated, ran->timed_out, ran->killed_by_signal,
            };
        }

        auto raw_lines = mtk::core::utils::split_lines_view(ran->stdout_data);
        std::size_t total_matches = raw_lines.size();

        std::unordered_map<std::string, std::vector<std::pair<std::size_t, std::string>>>
            by_file;
        for (const auto& line : raw_lines) {
            auto parsed = internal::parse_match_line(line);
            if (!parsed) continue;
            auto cleaned =
                internal::clean_line(parsed->content, lim::kMaxLineLen, cli.pattern);
            by_file[parsed->file].emplace_back(parsed->line_num, std::move(cleaned));
        }

        std::ostringstream out;
        out << total_matches << " matches in " << by_file.size() << " files:\n\n";

        std::vector<std::pair<std::string, std::vector<std::pair<std::size_t, std::string>>>>
            files(by_file.begin(), by_file.end());
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

        return mtk::core::exec::Ran{
            out.str(), std::string{}, ran->exit_code,
            ran->truncated, ran->timed_out, ran->killed_by_signal,
        };
    }
};

void register_builtins(mtk::core::Registry& reg) {
    reg.register_filter(std::make_unique<GrepFilter>(),
                        mtk::core::Tier::Builtin, /*is_final=*/true);
}

}  // namespace mtk::cmds::grep
