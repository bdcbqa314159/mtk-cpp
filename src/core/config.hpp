#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/toml_filter.hpp"

namespace mtk::core::config {

std::filesystem::path config_dir();
std::filesystem::path filters_dir();

// User filters from ~/.config/mtk/filters/*.toml. Always loaded (no gate).
std::vector<mtk::core::toml_filter::Filter> load_user_filters();

// Project filters from <cwd>/.mtk/filters/*.toml. Per A2: loaded only when
// cwd is in `mtk::core::trust::is_trusted`. When skipped, emits a stderr
// nag of the form `[mtk: project filters at ./.mtk/filters were not
// loaded; allow with: mtk trust .]` (only if the dir actually exists).
std::vector<mtk::core::toml_filter::Filter> load_project_filters();

// Convenience: load_user_filters() + load_project_filters() concatenated.
// Use the split variants when you need to know which tier a filter came
// from (e.g., for registry registration with the correct Tier).
std::vector<mtk::core::toml_filter::Filter> load_all_filters();

std::optional<mtk::core::toml_filter::Filter> find_filter_for(
    const std::string& cmd_name,
    const std::vector<mtk::core::toml_filter::Filter>& filters);

}  // namespace mtk::core::config
