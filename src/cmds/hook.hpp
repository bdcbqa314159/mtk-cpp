#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mtk::cmds::hook {

// Implements `mtk hook copilot` per the dual-format hook config used by
// VS Code Copilot Chat and Copilot CLI.
//
// Reads tool-input JSON from stdin. Detects which format (snake_case
// `tool_input.command` for VS Code, camelCase `toolArgs` JSON-string for
// CLI). Extracts the bash command, dispatches it through the registry to
// decide whether a non-passthrough filter would match, and emits the
// modified JSON to stdout. If no rewrite is warranted (passthrough only,
// or no Bash tool), echoes the input unchanged.
//
// Returns the canonical exit code for the caller (the agent) to propagate.
// Errors (malformed JSON, missing fields, etc.) are non-fatal: emit a
// stderr warning, pass input through unchanged, return 0.
[[nodiscard]] int run_copilot();

// Internal pure helpers — exposed for unit testing. Per CE6.
namespace internal {

[[nodiscard]] std::string_view strip_bom(std::string_view s) noexcept;
[[nodiscard]] std::vector<std::string> tokenise(std::string_view cmd);
[[nodiscard]] bool contains_shell_metachar(std::string_view cmd) noexcept;
[[nodiscard]] bool is_env_assignment(std::string_view tok) noexcept;
[[nodiscard]] bool is_transparent_wrapper(std::string_view tok) noexcept;
[[nodiscard]] std::size_t skip_leading_noops(const std::vector<std::string>& argv) noexcept;
[[nodiscard]] std::optional<std::string> decide_rewrite(const std::string& cmd);

}  // namespace internal

}  // namespace mtk::cmds::hook
