#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mtk::core::tee {

std::filesystem::path tee_dir();

std::optional<std::string> tee_and_hint(std::string_view raw,
                                        std::string_view command_slug,
                                        int exit_code);

}  // namespace mtk::core::tee
