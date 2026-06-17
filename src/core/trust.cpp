#include "core/trust.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "core/config.hpp"
#include "core/platform/process_id.hpp"

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

// Per correctness critic C8: write atomically via temp-file + rename(2).
// The previous truncate-then-write pattern left the allow-list empty
// (or torn) if mtk was killed mid-write, or if a concurrent reader hit
// the file between the truncate and the trailing writes. rename(2) on
// the same filesystem is atomic per POSIX, so a reader either sees the
// old file or the new file in full — never partial.
//
// Per audit (post-Phase-4): the tmp path is per-process via getpid() so
// two concurrent `mtk trust` invocations don't share a tmp file. The
// previous fixed `.mtk-tmp` suffix caused a *confused-success* race —
// A's tmp was wiped by B's `trunc` open, A's rename committed B's data,
// and A reported "added /A" while the file held /B. Per-PID tmps make
// the local-process write atomic; cross-process lost-update remains
// possible (the read-modify-write isn't locked) but is now a true
// last-writer-wins, not a lying-success.
bool write_file(const std::filesystem::path& path,
                const std::vector<std::filesystem::path>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    auto tmp = path;
    tmp += ".mtk-tmp." + std::to_string(mtk::core::platform::current_pid());

    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "# mtk allow-list — canonical paths whose .mtk/filters/ may load.\n"
          << "# Edit via `mtk trust <path>` / `mtk untrust <path>`.\n";
        for (const auto& e : entries) f << e.string() << '\n';
        if (!f.good()) {
            std::filesystem::remove(tmp, ec);  // best-effort cleanup
            return false;
        }
    }  // ofstream dtor flushes + closes before rename

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);  // best-effort cleanup
        return false;
    }
    return true;
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
