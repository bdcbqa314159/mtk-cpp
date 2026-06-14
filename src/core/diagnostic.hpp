#pragma once
#include <string_view>

namespace mtk::core::diag {

// Writes the canonical "mtk <tool>: <message>\n" diagnostic to stderr.
// Per CR5: every user-facing error from mtk uses this prefix shape.
void emit(std::string_view tool, std::string_view message) noexcept;

}  // namespace mtk::core::diag
