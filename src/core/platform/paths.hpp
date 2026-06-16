#pragma once

// Per-user and system-wide directory roots, abstracted across platforms.
// Replaces scattered `getenv("HOME")` + XDG_* logic with one canonical
// API. Each function returns the platform-correct directory; the caller
// appends subpaths like `/ filters` or `/ events.jsonl` as needed.
//
// POSIX path conventions follow the XDG Base Directory spec:
//   config_dir = $XDG_CONFIG_HOME/mtk        or  $HOME/.config/mtk
//   cache_dir  = $XDG_CACHE_HOME/mtk         or  $HOME/.cache/mtk
//   state_dir  = $XDG_STATE_HOME/mtk         or  $HOME/.local/state/mtk
//   org_dir    = $MTK_ORG_CONFIG             or  /etc/mtk
//
// Windows path conventions follow the Known Folders pattern:
//   config_dir = %APPDATA%\mtk               (Roaming user config)
//   cache_dir  = %LOCALAPPDATA%\mtk\cache    (machine-local rebuildable)
//   state_dir  = %LOCALAPPDATA%\mtk\state    (machine-local persistent)
//   org_dir    = $MTK_ORG_CONFIG             or  %PROGRAMDATA%\mtk
//
// home_dir() is exposed for the rare case where a caller needs the
// user's home root directly (e.g. `~/.claude/hooks/...` in
// `cmds/init.cpp` — that's Claude's path, not mtk's).

#include <filesystem>

namespace mtk::core::platform {

[[nodiscard]] std::filesystem::path home_dir();
[[nodiscard]] std::filesystem::path config_dir();
[[nodiscard]] std::filesystem::path cache_dir();
[[nodiscard]] std::filesystem::path state_dir();
[[nodiscard]] std::filesystem::path org_dir();

}  // namespace mtk::core::platform
