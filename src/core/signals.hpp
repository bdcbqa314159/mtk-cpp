#pragma once

namespace mtk::core::signals {

// Install POSIX signal handlers per A11. Idempotent — safe to call multiple
// times (only the first call installs).
//
//   SIGINT, SIGTERM, SIGHUP  →  record into a lock-free bitmask. NO
//                                SA_RESTART so blocked pipe reads inside
//                                reproc::drain return EINTR and we can
//                                react at the next poll boundary (rather
//                                than waiting up to capture.timeout_ms).
//   SIGPIPE                  →  immediate _Exit(141). Async-signal-safe.
//                                Per A11: SIGPIPE means the user explicitly
//                                stopped reading — we do not tee, do not
//                                audit, do not buffer-flush. SA_RESTART is
//                                set but irrelevant (the handler doesn't
//                                return).
//
// Process-group note: by default the child shares mtk's process group, so
// terminal signals (Ctrl-C) reach both. The bitmask handler records the
// signal so mtk doesn't die immediately; the main loop drains the child,
// propagates the exit code, and on top-level audit records the signal.
void install() noexcept;

// Atomically reads-and-clears the pending signal bitmask. Returns one of
// SIGINT/SIGTERM/SIGHUP (priority order) if pending, or 0. Replaces the
// older pending()/clear() pair which had a load-then-store race window
// where a fresh signal could be silently overwritten between the two
// calls (correctness critic C3).
[[nodiscard]] int take() noexcept;

// Non-destructive check — does not clear the bitmask.
[[nodiscard]] bool any_pending() noexcept;

}  // namespace mtk::core::signals
