#pragma once

// is_stdout_tty(): true iff this process's stdout is connected to an
// interactive terminal (vs. being piped to another process or
// redirected to a file). Underlies the color-policy decision in
// `core/color.hpp`: agents see plain output, humans see styled output.
//
// enable_vt_processing(): no-op on POSIX (terminals interpret ANSI
// natively). On Windows, enables `ENABLE_VIRTUAL_TERMINAL_PROCESSING`
// on the stdout console handle so emitted ANSI escapes actually
// render rather than printing as gibberish. Safe to call when stdout
// is not a console; returns false silently.

namespace mtk::core::platform {

[[nodiscard]] bool is_stdout_tty() noexcept;

bool enable_vt_processing() noexcept;

}  // namespace mtk::core::platform
