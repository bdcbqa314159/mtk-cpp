#include "core/config.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "core/trust.hpp"

namespace mtk::core::config {

namespace {

std::filesystem::path home_dir() {
    if (const char* h = std::getenv("HOME")) return std::filesystem::path(h);
    return std::filesystem::path(".");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

std::vector<mtk::core::toml_filter::Filter>
load_from_dir(const std::filesystem::path& dir) {
    std::vector<mtk::core::toml_filter::Filter> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".toml") continue;
        auto src = read_file(entry.path());
        if (src.empty()) continue;
        auto parsed = mtk::core::toml_filter::parse_all(src);
        for (auto& f : parsed) out.push_back(std::move(f));
    }
    return out;
}

}  // namespace

std::filesystem::path config_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "mtk";
    }
    return home_dir() / ".config" / "mtk";
}

std::filesystem::path filters_dir() {
    return config_dir() / "filters";
}

std::vector<mtk::core::toml_filter::Filter> load_user_filters() {
    return load_from_dir(filters_dir());
}

std::vector<mtk::core::toml_filter::Filter> load_project_filters() {
    auto project_dir = std::filesystem::path(".mtk") / "filters";
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return {};

    if (!mtk::core::trust::is_trusted(cwd)) {
        // Per A2: emit nag only if the directory exists (so trusted-but-empty
        // projects don't get spammed).
        std::error_code dir_ec;
        if (std::filesystem::is_directory(project_dir, dir_ec)) {
            std::cerr << "[mtk: project filters at " << project_dir
                      << " were not loaded; allow with: mtk trust .]\n";
        }
        return {};
    }
    return load_from_dir(project_dir);
}

std::vector<mtk::core::toml_filter::Filter> load_all_filters() {
    auto user = load_user_filters();
    auto project = load_project_filters();
    user.insert(user.end(),
                std::make_move_iterator(project.begin()),
                std::make_move_iterator(project.end()));
    return user;
}

std::optional<mtk::core::toml_filter::Filter> find_filter_for(
    const std::string& cmd_name,
    const std::vector<mtk::core::toml_filter::Filter>& filters) {
    for (const auto& f : filters) {
        if (mtk::core::toml_filter::command_matches(f, cmd_name)) return f;
    }
    return std::nullopt;
}

}  // namespace mtk::core::config
