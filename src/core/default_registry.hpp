#pragma once

#include "core/registry.hpp"

namespace mtk::core {

// Builds the default registry: TOML filters from ~/.config/mtk/filters
// and project .mtk/filters (subject to A2's allow-list — Phase 1.5 step 4
// adds the trust gate; for now project filters are loaded unconditionally
// to preserve existing behavior), plus the PassthroughFilter fallback.
//
// Built-in C++ filters (GitLogFilter, LsFilter, etc.) are not yet wired
// in — Phase 1.5 step 3 adds them. Until then, the dedicated git/ls/grep
// paths run via their CLI11 subcommand callbacks in main.cpp.
[[nodiscard]] Registry build_default_registry();

}  // namespace mtk::core
