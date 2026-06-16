#pragma once
#include <filesystem>

namespace mtk::core {

// Returns the absolute path of the currently-running mtk binary, or an
// empty path if it can't be resolved. Used by `mtk init <agent>` so the
// generated hook configs reference the actual binary by absolute path —
// crucial for build-only users who can't put mtk on the system PATH.
[[nodiscard]] std::filesystem::path executable_path() noexcept;

}  // namespace mtk::core
