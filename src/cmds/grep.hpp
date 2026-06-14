#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace mtk::cmds::grep {

int run(const std::vector<std::string>& args);

namespace internal {

struct ParsedMatch {
    std::string file;
    std::size_t line_num = 0;
    std::string content;
};

std::optional<ParsedMatch> parse_match_line(std::string_view line);

bool has_format_flag(const std::vector<std::string>& extra_args);

std::string clean_line(std::string_view raw,
                       std::size_t max_len,
                       std::string_view pattern);

std::string compact_path(std::string_view path);

std::string translate_bre_alternation(std::string_view pattern);

}  // namespace internal

}  // namespace mtk::cmds::grep
