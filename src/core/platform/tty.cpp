#include "core/platform/tty.hpp"

#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#  include <io.h>
#elif defined(__linux__) || defined(__APPLE__)
#  include <unistd.h>
#else
#  error "tty: unsupported platform"
#endif

namespace mtk::core::platform {

bool is_stdout_tty() noexcept {
#if defined(_WIN32)
    return ::_isatty(::_fileno(stdout)) != 0;
#elif defined(__linux__) || defined(__APPLE__)
    return ::isatty(::fileno(stdout)) != 0;
#endif
}

bool enable_vt_processing() noexcept {
#if defined(_WIN32)
    // Modern Windows 10+ / Windows Terminal / conhost all support ANSI
    // when ENABLE_VIRTUAL_TERMINAL_PROCESSING is set. Older conhost
    // ignores the flag; SetConsoleMode returns false; we degrade
    // gracefully (callers should also gate emission on is_stdout_tty()
    // which catches non-console handles).
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
    DWORD mode = 0;
    if (!::GetConsoleMode(h, &mode)) return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;  // already on
    return ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#elif defined(__linux__) || defined(__APPLE__)
    return true;  // POSIX terminals interpret ANSI natively
#endif
}

}  // namespace mtk::core::platform
