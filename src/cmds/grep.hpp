#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/registry.hpp"

namespace mtk::cmds::grep {

// Registers all grep-related filters into the given registry at Tier::Builtin
// with is_final=true.
void register_builtins(mtk::core::Registry& reg);

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
