#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mtk::core::toml_filter {

struct Filter {
    std::string name;
    std::string description;
    std::string match_command_pattern;
    bool filter_stderr = false;
    bool strip_ansi = false;
    std::vector<std::pair<std::string, std::string>> replace;
    std::optional<std::pair<std::string, std::string>> match_output;
    std::vector<std::string> strip_lines_matching;
    std::vector<std::string> keep_lines_matching;
    std::optional<std::size_t> truncate_lines_at;
    std::optional<std::size_t> head_lines;
    std::optional<std::size_t> tail_lines;
    std::optional<std::size_t> max_lines;
    std::optional<std::string> on_empty;
};

std::vector<Filter> parse_all(std::string_view toml_source);

bool command_matches(const Filter& f, std::string_view cmd_name);

std::string apply(const Filter& f, std::string_view input);

}  // namespace mtk::core::toml_filter
