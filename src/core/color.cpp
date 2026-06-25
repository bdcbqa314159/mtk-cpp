#include "core/color.hpp"

#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

#include "core/platform/tty.hpp"
#include "core/utils.hpp"

namespace mtk::core::color {

namespace {

bool resolve_policy() noexcept {
    // NO_COLOR: any non-empty value disables. The de-facto standard
    // (no-color.org) says "presence of the variable, regardless of
    // value"; we use non-empty for parity with how `MTK_COLOR` reads.
    if (const char* nc = std::getenv("NO_COLOR")) {
        if (*nc != '\0') return false;
    }
    if (const char* mc = std::getenv("MTK_COLOR")) {
        const std::string_view v(mc);
        if (v == "never")  return false;
        if (v == "always") return true;
        // "auto" or any other value: fall through to TTY detect.
    }
    return mtk::core::platform::is_stdout_tty();
}

}  // namespace

bool colors_enabled() noexcept {
    // Resolve once. std::call_once is cheap after first call and
    // thread-safe; mtk is single-threaded today but this is the
    // canonical idiom and costs ~one atomic load steady-state.
    static bool enabled = false;
    static std::once_flag flag;
    std::call_once(flag, [] {
        enabled = resolve_policy();
        // When we're going to emit, make sure Windows console can
        // render the escapes. POSIX is a no-op.
        if (enabled) {
            (void)mtk::core::platform::enable_vt_processing();
        }
    });
    return enabled;
}

std::string strip(std::string_view input) {
    return mtk::core::utils::strip_ansi(input);
}

namespace {

// Reset sequence applied at the tail of every wrapper.
constexpr std::string_view kReset = "\x1b[0m";

std::string wrap(std::string_view code, std::string_view s) {
    if (!colors_enabled()) return std::string(s);
    std::string out;
    out.reserve(code.size() + s.size() + kReset.size());
    out.append(code.data(), code.size());
    out.append(s.data(), s.size());
    out.append(kReset.data(), kReset.size());
    return out;
}

}  // namespace

std::string red    (std::string_view s) { return wrap("\x1b[31m", s); }
std::string green  (std::string_view s) { return wrap("\x1b[32m", s); }
std::string yellow (std::string_view s) { return wrap("\x1b[33m", s); }
std::string blue   (std::string_view s) { return wrap("\x1b[34m", s); }
std::string magenta(std::string_view s) { return wrap("\x1b[35m", s); }
std::string cyan   (std::string_view s) { return wrap("\x1b[36m", s); }
std::string dim    (std::string_view s) { return wrap("\x1b[2m",  s); }
std::string bold   (std::string_view s) { return wrap("\x1b[1m",  s); }

}  // namespace mtk::core::color
