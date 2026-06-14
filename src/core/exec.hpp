#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mtk::core::exec {

using EnvExtra = std::vector<std::pair<std::string, std::string>>;

// --- legacy API (to be removed once all cmd files migrate) ---

struct CapturedOutput {
    std::string stdout_data;
    std::string stderr_data;
    int exit_code = 0;
    bool spawned = false;
    std::string spawn_error;
};

CapturedOutput capture(const std::vector<std::string>& argv,
                       const EnvExtra& env_extra = {});

int passthrough(const std::vector<std::string>& argv);

// --- new API (per A3): two-arm sum type, no two-truth ambiguity ---

struct SpawnFailed {
    std::string message;
};

struct Ran {
    std::string stdout_data;
    std::string stderr_data;
    int exit_code = 0;
    [[nodiscard]] bool clean() const noexcept { return exit_code == 0; }
};

using ExecOutcome = std::variant<SpawnFailed, Ran>;

// Capture stdout+stderr from a child process. Returns SpawnFailed when:
//   - reproc::process::start fails (fork failure, exec PATH lookup fails
//     reported synchronously, etc.)
//   - the child returns exit 127 with no stdout — heuristic for execvp
//     failing inside the child (which reproc reports as "spawned but
//     immediately exit-127" since the fork succeeded). This is the
//     case the old CapturedOutput contract couldn't distinguish.
[[nodiscard]] ExecOutcome capture_outcome(const std::vector<std::string>& argv,
                                          const EnvExtra& env_extra = {});

}  // namespace mtk::core::exec
