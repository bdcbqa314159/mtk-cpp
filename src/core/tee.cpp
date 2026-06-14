#include "core/tee.hpp"

#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>

namespace mtk::core::tee {

namespace {

std::filesystem::path home_dir() {
    if (const char* h = std::getenv("HOME")) return std::filesystem::path(h);
    return std::filesystem::path(".");
}

std::string slugify(std::string_view cmd) {
    std::string out;
    out.reserve(cmd.size());
    for (char c : cmd) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += c;
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    if (out.empty()) out = "cmd";
    if (out.size() > 40) out.resize(40);
    return out;
}

}  // namespace

std::filesystem::path tee_dir() {
    auto dir = home_dir() / ".local" / "share" / "mtk" / "tee";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::optional<std::string> tee_and_hint(std::string_view raw,
                                        std::string_view command_slug,
                                        int exit_code) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    auto path = tee_dir() /
        fmt::format("{}_{}_{}.log", secs, slugify(command_slug), exit_code);

    std::ofstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    f.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (!f) return std::nullopt;

    return fmt::format("[mtk: full output saved to {}]", path.string());
}

}  // namespace mtk::core::tee
