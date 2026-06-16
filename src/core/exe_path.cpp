#include "core/exe_path.hpp"

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#elif defined(_WIN32)
#  include <windows.h>
#endif

namespace mtk::core {

std::filesystem::path executable_path() noexcept {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = std::filesystem::canonical(buf, ec);
        if (!ec) return p;
    }
#elif defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return p;
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::filesystem::path(buf);
#endif
    return {};
}

}  // namespace mtk::core
