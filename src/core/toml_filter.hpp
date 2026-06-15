#pragma once
#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mtk::core::toml_filter {

// Per perf critic P1: regex patterns are PRECOMPILED at parse_all() time
// rather than per-line per-filter per-invocation. Both the string form
// and the compiled std::regex are stored — the string for diagnostics
// and serialization, the regex for the hot path.
struct Filter {
    std::string name;
    std::string description;
    std::string match_command_pattern;
    bool filter_stderr = false;
    bool strip_ansi = false;

    // Pattern-strings (source of truth for serialisation, diagnostics).
    std::vector<std::pair<std::string, std::string>> replace;
    std::optional<std::pair<std::string, std::string>> match_output;
    std::vector<std::string> strip_lines_matching;
    std::vector<std::string> keep_lines_matching;

    std::optional<std::size_t> truncate_lines_at;
    std::optional<std::size_t> head_lines;
    std::optional<std::size_t> tail_lines;
    std::optional<std::size_t> max_lines;
    std::optional<std::string> on_empty;

    // --- Compiled-once regex parallels populated by parse_all() (Round B1). ---
    // Any pattern that fails to compile is skipped (the corresponding entry
    // is absent from the *_re container). apply() and command_matches()
    // iterate the *_re containers, not the string forms.
    std::optional<std::regex> match_command_re;
    std::vector<std::pair<std::regex, std::string>> replace_re;
    std::optional<std::pair<std::regex, std::string>> match_output_re;
    std::vector<std::regex> strip_lines_re;
    std::vector<std::regex> keep_lines_re;
};

std::vector<Filter> parse_all(std::string_view toml_source);

// Populate the *_re fields from the pattern-string fields. Called by
// parse_all() automatically; callers that construct Filter manually (tests,
// programmatic use) must call this before command_matches / apply. Bad
// patterns are skipped (no exception) — same loose semantics as before.
void compile(Filter& f);

bool command_matches(const Filter& f, std::string_view cmd_name);

std::string apply(const Filter& f, std::string_view input);

}  // namespace mtk::core::toml_filter
