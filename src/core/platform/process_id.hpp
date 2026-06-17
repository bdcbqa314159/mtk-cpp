#pragma once

// current_pid(): platform-portable wrapper around getpid() /
// GetCurrentProcessId(). Returns a numeric ID stable for the lifetime of
// the process — used as a per-PID temp-filename suffix (trust, tee,
// filter_cache, audit-save tmps) and as a coarse-randomness seed
// component (audit::make_event_id).
//
// std::int64_t is wide enough for any platform's PID type and avoids
// sign-mismatch noise at call sites (DWORD on Windows, pid_t on POSIX).

#include <cstdint>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#  include <unistd.h>
#else
#  error "process_id: unsupported platform"
#endif

namespace mtk::core::platform {

[[nodiscard]] inline std::int64_t current_pid() noexcept {
#if defined(_WIN32)
    return static_cast<std::int64_t>(::GetCurrentProcessId());
#elif defined(__linux__) || defined(__APPLE__)
    return static_cast<std::int64_t>(::getpid());
#endif
}

}  // namespace mtk::core::platform
