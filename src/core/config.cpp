#include "core/config.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/platform/paths.hpp"
#include "core/color.hpp"
#include "core/trust.hpp"

namespace mtk::core::config {

namespace {

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
    // Per correctness critic C-RoundE-6: the `ec` arg to directory_iterator
    // only suppresses *construction* failures; range-for's operator++ still
    // throws on mid-iteration errors (broken symlink, permissions stripped
    // between stat calls). That exception escaped dispatch() and killed
    // mtk. Wrap in try/catch + per-entry ec-overload of is_regular_file.
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
            if (entry.path().extension() != ".toml") continue;
            auto src = read_file(entry.path());
            if (src.empty()) continue;
            auto parsed = mtk::core::toml_filter::parse_all(src);
            for (auto& f : parsed) out.push_back(std::move(f));
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Best-effort: return whatever we collected before the iteration
        // error. Caller treats this no differently from a missing dir.
    }
    return out;
}

}  // namespace

std::filesystem::path config_dir() {
    return mtk::core::platform::config_dir();
}

std::filesystem::path filters_dir() {
    return config_dir() / "filters";
}

std::filesystem::path org_dir() {
    return mtk::core::platform::org_dir();
}

std::filesystem::path org_filters_dir() {
    return org_dir() / "filters";
}

std::vector<mtk::core::toml_filter::Filter> load_user_filters() {
    return load_from_dir(filters_dir());
}

std::vector<mtk::core::toml_filter::Filter> load_org_filters() {
    return load_from_dir(org_filters_dir());
}

std::vector<mtk::core::toml_filter::Filter> load_project_filters() {
    auto project_dir = std::filesystem::path(".mtk") / "filters";
    // Per perf critic P4 (Round F): cheap stat first. is_trusted reads
    // the allow-list file from disk; skip it entirely when the project
    // has no .mtk/filters (the common case for any non-mtk-using repo).
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(project_dir, dir_ec)) return {};

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return {};

    if (!mtk::core::trust::is_trusted(cwd)) {
        // Yellow on TTY — this is a warning the user should notice
        // (they have project filters but they're not loading) but it's
        // not an error.
        std::ostringstream nag;
        nag << "[mtk: project filters at " << project_dir
            << " were not loaded; allow with: mtk trust .]";
        std::cerr << mtk::core::color::yellow(nag.str()) << '\n';
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

namespace {
std::vector<std::filesystem::path> list_toml_paths(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    // Same throw-on-iteration concern as load_from_dir — see comment there.
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
            if (entry.path().extension() != ".toml") continue;
            out.push_back(entry.path());
        }
    } catch (const std::filesystem::filesystem_error&) {
        // Best-effort.
    }
    std::sort(out.begin(), out.end());  // stable order for cache manifest
    return out;
}
}  // namespace

std::vector<std::filesystem::path> user_filter_paths() {
    return list_toml_paths(filters_dir());
}

std::vector<std::filesystem::path> org_filter_paths() {
    return list_toml_paths(org_filters_dir());
}

std::vector<std::filesystem::path> project_filter_paths() {
    auto project_dir = std::filesystem::path(".mtk") / "filters";
    // Per perf critic P4 (Round F): see comment in load_project_filters.
    std::error_code dir_ec;
    if (!std::filesystem::is_directory(project_dir, dir_ec)) return {};

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return {};
    if (!mtk::core::trust::is_trusted(cwd)) return {};
    return list_toml_paths(project_dir);
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
