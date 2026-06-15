#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mtk::core::utils {

// --- string transformations ---

[[nodiscard]] std::string strip_ansi(std::string_view input);

// UTF-8 boundary-safe; if max_len is reached, appends "...".
[[nodiscard]] std::string truncate(std::string_view input, std::size_t max_len);

// --- string predicates and slicing ---

[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix) noexcept;

[[nodiscard]] std::string trim_copy(std::string_view s);

[[nodiscard]] std::string to_lower(std::string_view s);

// --- line splitting / joining ---

[[nodiscard]] std::vector<std::string> split_lines(std::string_view input);

// Per perf critic P9: zero-allocation variant. Each returned view slices
// into `input` — the caller MUST keep `input` alive for the lifetime of
// the returned vector. Use this on hot paths that only read; prefer
// `split_lines` when callers move/mutate the line strings.
[[nodiscard]] std::vector<std::string_view> split_lines_view(std::string_view input);

[[nodiscard]] std::string join_lines(const std::vector<std::string>& lines);

[[nodiscard]] std::size_t count_tokens(std::string_view input);

}  // namespace mtk::core::utils
