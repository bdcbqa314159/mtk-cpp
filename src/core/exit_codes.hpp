#pragma once
#include <string_view>

namespace mtk::core::exit_codes {

// Canonical exit codes per CR6. Tool-propagated codes pass through unchanged.
inline constexpr int kSuccess = 0;
inline constexpr int kUsage = 2;
inline constexpr int kNotExecutable = 126;
inline constexpr int kNotFound = 127;
inline constexpr int kSigPipe = 141;  // 128 + SIGPIPE(13)

// Emit a canonical "mtk <tool>: failed to spawn: <error>" diagnostic and
// return 127. Single source of truth for the spawn-failure boilerplate
// previously duplicated across every run_* in cmds/*.cpp.
[[nodiscard]] int report_spawn_failure(std::string_view tool,
                                       std::string_view error) noexcept;

}  // namespace mtk::core::exit_codes
