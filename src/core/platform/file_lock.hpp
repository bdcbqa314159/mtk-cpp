#pragma once

// Cross-platform exclusive file locking + file-identity comparison.
//
// audit.cpp's rotation protocol depends on two things:
//   1. Take an exclusive lock that blocks across processes.
//   2. After locking, check whether the path we opened still refers to
//      the same on-disk file (someone else may have rotated it between
//      our open() and our lock acquisition).
//
// POSIX uses `flock(fd, LOCK_EX)` + `stat::st_ino`/`st_dev` comparison.
// Windows uses `LockFileEx` + `GetFileInformationByHandle` returning
// `(VolumeSerialNumber, FileIndexHigh, FileIndexLow)`. Different APIs,
// same semantics; this header papers over the difference.
//
// The native file-handle type leaks into the public surface because the
// caller already owns the open file (via ::open / CreateFile) and we
// don't want to take ownership here. `kInvalidFileHandle` is the
// platform's "no handle" sentinel.

#include <filesystem>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

namespace mtk::core::platform {

#if defined(_WIN32)
using native_file_handle = HANDLE;
inline const native_file_handle kInvalidFileHandle = INVALID_HANDLE_VALUE;
#elif defined(__linux__) || defined(__APPLE__)
using native_file_handle = int;
inline constexpr native_file_handle kInvalidFileHandle = -1;
#else
#  error "file_lock: unsupported platform"
#endif

// Acquire an exclusive advisory lock on `fd`. Blocks until acquired.
// Returns true on success; on failure the caller should close + bail.
[[nodiscard]] bool lock_exclusive(native_file_handle fd) noexcept;

// Release a previously-acquired exclusive lock. Best-effort: errors are
// swallowed (caller is about to close the handle anyway).
void unlock(native_file_handle fd) noexcept;

// True iff `fd` and `path` refer to the same on-disk file.
//
// On POSIX: identical (st_dev, st_ino).
// On Windows: identical (dwVolumeSerialNumber, nFileIndexHigh, nFileIndexLow).
//
// Returns false on any stat / handle-info failure (caller treats as
// "different" — the safer default for rotation-detection use cases).
[[nodiscard]] bool same_file(native_file_handle fd,
                             const std::filesystem::path& path) noexcept;

}  // namespace mtk::core::platform
