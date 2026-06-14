#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mtk::core::utils {

std::string strip_ansi(std::string_view input);

std::string truncate(std::string_view input, std::size_t max_len);

std::vector<std::string> split_lines(std::string_view input);

std::string join_lines(const std::vector<std::string>& lines);

std::size_t count_tokens(std::string_view input);

}  // namespace mtk::core::utils
