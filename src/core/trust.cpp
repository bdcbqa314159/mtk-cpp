#include "core/trust.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "core/config.hpp"

namespace mtk::core::trust {

namespace {

bool env_flag_set() noexcept {
    if (const char* v = std::getenv("MTK_ALLOW_PROJECT_FILTERS")) {
        return std::string_view(v) == "1";
    }
    return false;
}

std::vector<std::filesystem::path> read_file_unfiltered(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line.front() == '#') continue;
        out.emplace_back(line);
    }
    return out;
}

bool write_file(const std::filesystem::path& path,
                const std::vector<std::filesystem::path>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# mtk allow-list — canonical paths whose .mtk/filters/ may load.\n"
      << "# Edit via `mtk trust <path>` / `mtk untrust <path>`.\n";
    for (const auto& e : entries) f << e.string() << '\n';
    return f.good();
}

}  // namespace

std::filesystem::path allowed_projects_file() {
    return mtk::core::config::config_dir() / "allowed_projects";
}

std::filesystem::path canonicalise(const std::filesystem::path& p) {
    std::error_code ec;
    auto out = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
        std::cerr << "mtk trust: could not canonicalise " << p << ": "
                  << ec.message() << '\n';
        return {};
    }
    return out;
}

bool is_trusted(const std::filesystem::path& p) {
    if (env_flag_set()) return true;
    auto target = canonicalise(p);  // local fn, see trust.hpp
    if (target.empty()) return false;
    auto entries = read_file_unfiltered(allowed_projects_file());
    for (const auto& e : entries) {
        if (e == target) return true;
    }
    return false;
}

bool add(const std::filesystem::path& p) {
    auto target = canonicalise(p);  // local fn, see trust.hpp
    if (target.empty()) return false;
    auto entries = read_file_unfiltered(allowed_projects_file());
    for (const auto& e : entries) {
        if (e == target) return false;  // already present
    }
    entries.push_back(target);
    return write_file(allowed_projects_file(), entries);
}

bool remove(const std::filesystem::path& p) {
    auto target = canonicalise(p);  // local fn, see trust.hpp
    if (target.empty()) return false;
    auto entries = read_file_unfiltered(allowed_projects_file());
    auto before = entries.size();
    entries.erase(std::remove(entries.begin(), entries.end(), target),
                  entries.end());
    if (entries.size() == before) return false;
    return write_file(allowed_projects_file(), entries);
}

std::vector<std::filesystem::path> list() {
    return read_file_unfiltered(allowed_projects_file());
}

}  // namespace mtk::core::trust
