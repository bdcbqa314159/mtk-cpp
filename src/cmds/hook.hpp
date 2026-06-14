#pragma once
#include <string>
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

}  // namespace mtk::cmds::hook
