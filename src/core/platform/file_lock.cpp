#include "core/platform/file_lock.hpp"

#if defined(_WIN32)
// Windows.h pulled in via the header.
#elif defined(__linux__) || defined(__APPLE__)
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace mtk::core::platform {

bool lock_exclusive(native_file_handle fd) noexcept {
#if defined(_WIN32)
    // LockFileEx with LOCKFILE_EXCLUSIVE_LOCK (no LOCKFILE_FAIL_IMMEDIATELY)
    // blocks until acquired. Range MAXDWORD/MAXDWORD locks the whole file.
    OVERLAPPED ov{};
    return ::LockFileEx(fd, LOCKFILE_EXCLUSIVE_LOCK,
                        0, MAXDWORD, MAXDWORD, &ov) != 0;
#elif defined(__linux__) || defined(__APPLE__)
    return ::flock(fd, LOCK_EX) == 0;
#endif
}

void unlock(native_file_handle fd) noexcept {
#if defined(_WIN32)
    OVERLAPPED ov{};
    (void)::UnlockFileEx(fd, 0, MAXDWORD, MAXDWORD, &ov);
#elif defined(__linux__) || defined(__APPLE__)
    (void)::flock(fd, LOCK_UN);
#endif
}

bool same_file(native_file_handle fd,
               const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION fd_info{};
    if (!::GetFileInformationByHandle(fd, &fd_info)) return false;

    // Open the path metadata-only. FILE_FLAG_BACKUP_SEMANTICS works for
    // both files and directories. Read-share everything so we never
    // block other openers.
    HANDLE path_handle = ::CreateFileW(
        path.wstring().c_str(),
        0,  // no data access — metadata only
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (path_handle == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION path_info{};
    const bool ok = ::GetFileInformationByHandle(path_handle, &path_info) != 0;
    ::CloseHandle(path_handle);
    if (!ok) return false;

    return fd_info.dwVolumeSerialNumber == path_info.dwVolumeSerialNumber
        && fd_info.nFileIndexHigh == path_info.nFileIndexHigh
        && fd_info.nFileIndexLow == path_info.nFileIndexLow;
#elif defined(__linux__) || defined(__APPLE__)
    struct ::stat fd_st{};
    struct ::stat path_st{};
    if (::fstat(fd, &fd_st) != 0) return false;
    if (::stat(path.c_str(), &path_st) != 0) return false;
    return fd_st.st_ino == path_st.st_ino && fd_st.st_dev == path_st.st_dev;
#endif
}

}  // namespace mtk::core::platform
