#pragma once

// Color policy + ANSI escape helpers. One source of truth for "should
// we emit ANSI codes right now?" — every caller funnels through
// `colors_enabled()`, every escape is wrapped by a small helper that
// returns the input unchanged when colors are off.
//
// Resolution order (highest priority first):
//   1. NO_COLOR env set (any non-empty value) → off. (https://no-color.org)
//   2. MTK_COLOR=never                          → off.
//   3. MTK_COLOR=always                         → on.
//   4. MTK_COLOR=auto / unset                   → on iff stdout is a TTY.
//
// Resolved once per process and memoized.

#include <string>
#include <string_view>

namespace mtk::core::color {

[[nodiscard]] bool colors_enabled() noexcept;

// Pure policy resolution behind colors_enabled() — exposed so the decision
// matrix is unit-testable without mutating process env or memoised state.
// `no_color`/`mtk_color` are the raw env values (nullptr = unset); `is_tty`
// is whether stdout is a terminal. Order: NO_COLOR (non-empty) → off;
// MTK_COLOR never → off / always → on; otherwise follow `is_tty`.
[[nodiscard]] bool resolve_policy(const char* no_color, const char* mtk_color,
                                  bool is_tty) noexcept;

// Strip every ANSI escape sequence (CSI + OSC) from `input`. Cheap
// fast-path for ANSI-free input (returns a copy without running the
// regex). Always available — independent of colors_enabled().
[[nodiscard]] std::string strip(std::string_view input);

// Wrap `s` in an ANSI sequence iff colors_enabled(); otherwise return
// `s` unchanged. The trailing reset `\x1b[0m` is included so callers
// don't have to remember it.
[[nodiscard]] std::string red    (std::string_view s);
[[nodiscard]] std::string green  (std::string_view s);
[[nodiscard]] std::string yellow (std::string_view s);
[[nodiscard]] std::string blue   (std::string_view s);
[[nodiscard]] std::string cyan   (std::string_view s);
[[nodiscard]] std::string magenta(std::string_view s);
[[nodiscard]] std::string dim    (std::string_view s);
[[nodiscard]] std::string bold   (std::string_view s);

}  // namespace mtk::core::color
