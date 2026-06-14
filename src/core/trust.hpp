#pragma once
#include <filesystem>
#include <vector>

namespace mtk::core::trust {

// Per A2: project filters (`.mtk/filters/*.toml`) are loaded ONLY when the
// project's cwd is in this allow-list, OR when MTK_ALLOW_PROJECT_FILTERS=1.
// Default-off prevents the silent-shadow attack: a cloned repo with a
// `.mtk/filters/git.toml` would otherwise rewrite `mtk git status` on first
// `cd` into it.
//
// The allow-list file is `~/.config/mtk/allowed_projects` (one canonical
// absolute path per line; lines starting with `#` are comments).

[[nodiscard]] std::filesystem::path allowed_projects_file();

// Canonicalises `p` (resolves symlinks, makes absolute). Returns empty path
// on error (logs to stderr). Name avoids collision with std::filesystem::canonical.
[[nodiscard]] std::filesystem::path canonicalise(const std::filesystem::path& p);

// True if the env var MTK_ALLOW_PROJECT_FILTERS=1 OR if `p` (canonicalised)
// is listed in the allow-list file.
[[nodiscard]] bool is_trusted(const std::filesystem::path& p);

// Adds `p` (canonicalised) to the allow-list. Returns true if newly added,
// false if already present or on I/O error.
bool add(const std::filesystem::path& p);

// Removes `p` (canonicalised) from the allow-list. Returns true if removed,
// false if absent.
bool remove(const std::filesystem::path& p);

// Returns the current list of trusted canonical paths.
[[nodiscard]] std::vector<std::filesystem::path> list();

}  // namespace mtk::core::trust
