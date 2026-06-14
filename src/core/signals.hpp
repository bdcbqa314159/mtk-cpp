#pragma once

namespace mtk::core::signals {

// Install POSIX signal handlers per A11. Idempotent — safe to call multiple
// times (only the first call installs).
//
//   SIGINT, SIGTERM, SIGHUP  →  set a pending-signal flag; main-loop polling
//                                in capture/passthrough reacts (terminate
//                                child gracefully, then propagate 128+signo).
//   SIGPIPE                  →  immediate _exit(141). Async-signal-safe.
//                                Per A11: SIGPIPE means the user explicitly
//                                stopped reading — we do not tee, do not
//                                audit, do not buffer-flush.
//
// Process-group note: by default the child shares mtk's process group, so
// terminal signals (Ctrl-C) reach both. We install handlers so mtk doesn't
// die immediately and can wait for the child to exit + propagate.
void install() noexcept;

// Returns the most recent SIGINT/SIGTERM/SIGHUP signo (0 if none pending).
// Async-signal-safe via std::atomic<int>.
[[nodiscard]] int pending() noexcept;

// Resets the pending-signal flag. Called by RunContext after handling.
void clear() noexcept;

}  // namespace mtk::core::signals
