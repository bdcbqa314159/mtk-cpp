#include "core/tee.hpp"

#include <chrono>
#include <fmt/format.h>
#include <fstream>

#include "core/color.hpp"
#include "core/limits.hpp"
#include "core/platform/paths.hpp"
#include "core/platform/process_id.hpp"

namespace mtk::core::tee {

namespace {

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
    if (out.size() > mtk::core::limits::tee::kSlugMaxLen) {
        out.resize(mtk::core::limits::tee::kSlugMaxLen);
    }
    return out;
}

}  // namespace

std::filesystem::path tee_dir() {
    // Per the windows-port refactor: tee dumps now live under the
    // platform state dir (POSIX: ~/.local/state/mtk/tee; Windows:
    // %LOCALAPPDATA%\mtk\state\tee). Previously hardcoded as
    // ~/.local/share/mtk/tee — strictly XDG_DATA_HOME territory, but
    // these dumps are transient failure logs, semantically "state". Old
    // dumps at the previous location are not migrated; tee output is
    // throwaway by design.
    auto dir = mtk::core::platform::state_dir() / "tee";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::optional<std::string> tee_and_hint(std::string_view raw,
                                        std::string_view command_slug,
                                        int exit_code) {
    // Per correctness critic C10: the previous filename
    // `{secs}_{slug}_{exit}.log` collided whenever two failures of the
    // same command happened within the same second — the second write
    // silently overwrote the first. Use nanosecond resolution + PID so
    // the filename is unique both within and across mtk processes.
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    auto pid = mtk::core::platform::current_pid();

    auto path = tee_dir() /
        fmt::format("{}_{}_{}_{}.log", ns, pid, slugify(command_slug), exit_code);

    std::ofstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    f.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (!f) return std::nullopt;

    return mtk::core::color::dim(
        fmt::format("[mtk: full output saved to {}]", path.string()));
}

}  // namespace mtk::core::tee
