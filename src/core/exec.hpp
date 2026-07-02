#pragma once
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mtk::core::exec {

using EnvExtra = std::vector<std::pair<std::string, std::string>>;

// Per A3: two-arm sum type. The structural distinction is spawn-failed-vs-ran;
// "exited non-zero" is a runtime predicate (`Ran::clean()`), not a type-level
// distinction. Per CE5: no std::get/get_if/visit/index() on ExecOutcome outside
// this file — call sites go through RunContext.
struct SpawnFailed {
    std::string message;
};

struct Ran {
    std::string stdout_data;
    std::string stderr_data;
    int exit_code = 0;
    // Per A6: capture exceeded `limits::capture::kMaxBytes`; child was
    // terminated, the buffers above are truncated at the cap.
    bool truncated = false;
    // Per A6: capture wall-clock exceeded `limits::capture::kTimeoutMs`;
    // child was SIGTERM'd then SIGKILL'd after the grace period.
    bool timed_out = false;
    // Per A11: child was killed by a signal forwarded from mtk (Ctrl-C,
    // SIGTERM, etc.). exit_code is set to 128 + signo.
    int killed_by_signal = 0;
    [[nodiscard]] bool clean() const noexcept { return exit_code == 0; }
};

using ExecOutcome = std::variant<SpawnFailed, Ran>;

// Capture stdout+stderr from a child process. Returns SpawnFailed when:
//   - argv is empty;
//   - the binary cannot be resolved on PATH (pre-spawn check);
//   - reproc::process::start fails (fork/setup failure);
//   - the child returns exit 127 with no output (execvp inside the child
//     failed — reproc reports this as "spawned but immediately exited 127"
//     since the fork itself succeeded).
[[nodiscard]] ExecOutcome capture_outcome(const std::vector<std::string>& argv,
                                          const EnvExtra& env_extra = {});

// Spawn with parent stdio inherited. Returns the child's exit code, or 127
// for spawn/PATH failures (with a diagnostic emitted to stderr).
int passthrough(const std::vector<std::string>& argv);

// Rewrite argv so a Windows .cmd/.bat shim (npm, yarn, pnpm, tsc, ...) can be
// spawned. reproc's CreateProcessW only auto-appends .exe, so bare shim names
// never resolve; this searches PATH and, on a batch-file match, prepends
// `cmd /c`. .exe matches and POSIX are returned unchanged. Exposed for tests.
std::vector<std::string> resolve_launcher(const std::vector<std::string>& argv);

}  // namespace mtk::core::exec
