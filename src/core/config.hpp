#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/toml_filter.hpp"

namespace mtk::core::config {

std::filesystem::path config_dir();
std::filesystem::path filters_dir();

std::vector<mtk::core::toml_filter::Filter> load_all_filters();

std::optional<mtk::core::toml_filter::Filter> find_filter_for(
    const std::string& cmd_name,
    const std::vector<mtk::core::toml_filter::Filter>& filters);

}  // namespace mtk::core::config
