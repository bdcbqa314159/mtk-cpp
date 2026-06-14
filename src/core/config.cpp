#include "core/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

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

std::vector<mtk::core::toml_filter::Filter> load_all_filters() {
    std::vector<mtk::core::toml_filter::Filter> all;

    auto cwd_filters = std::filesystem::path(".mtk") / "filters";
    auto global_filters = filters_dir();

    for (const auto& dir : {cwd_filters, global_filters}) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".toml") continue;
            auto src = read_file(entry.path());
            if (src.empty()) continue;
            auto parsed = mtk::core::toml_filter::parse_all(src);
            for (auto& f : parsed) all.push_back(std::move(f));
        }
    }
    return all;
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
