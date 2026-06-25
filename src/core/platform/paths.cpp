#include "core/platform/paths.hpp"

#include <cstdlib>
#include <string>

namespace mtk::core::platform {

namespace {

#if defined(_WIN32)
// Compose %HOMEDRIVE%%HOMEPATH% when %USERPROFILE% is unset.
std::filesystem::path windows_compose_home() {
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && path) return std::filesystem::path(std::string(drive) + path);
    return std::filesystem::path(".");
}
#endif

}  // namespace

std::filesystem::path home_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) {
        return std::filesystem::path(p);
    }
    return windows_compose_home();
#elif defined(__linux__) || defined(__APPLE__)
    if (const char* h = std::getenv("HOME")) {
        return std::filesystem::path(h);
    }
    return std::filesystem::path(".");
#else
#  error "paths: unsupported platform"
#endif
}

std::filesystem::path config_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("APPDATA")) {
        return std::filesystem::path(p) / "mtk";
    }
    return home_dir() / "AppData" / "Roaming" / "mtk";
#elif defined(__linux__) || defined(__APPLE__)
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "mtk";
    }
    return home_dir() / ".config" / "mtk";
#endif
}

std::filesystem::path cache_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(p) / "mtk" / "cache";
    }
    return home_dir() / "AppData" / "Local" / "mtk" / "cache";
#elif defined(__linux__) || defined(__APPLE__)
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path(xdg) / "mtk";
    }
    return home_dir() / ".cache" / "mtk";
#endif
}

std::filesystem::path state_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(p) / "mtk" / "state";
    }
    return home_dir() / "AppData" / "Local" / "mtk" / "state";
#elif defined(__linux__) || defined(__APPLE__)
    if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
        return std::filesystem::path(xdg) / "mtk";
    }
    return home_dir() / ".local" / "state" / "mtk";
#endif
}

std::filesystem::path org_dir() {
    if (const char* env = std::getenv("MTK_ORG_CONFIG")) {
        return std::filesystem::path(env);
    }
#if defined(_WIN32)
    if (const char* p = std::getenv("PROGRAMDATA")) {
        return std::filesystem::path(p) / "mtk";
    }
    return std::filesystem::path("C:/ProgramData/mtk");
#elif defined(__linux__) || defined(__APPLE__)
    return std::filesystem::path("/etc/mtk");
#endif
}

}  // namespace mtk::core::platform
