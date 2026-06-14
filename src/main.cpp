#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#elif defined(_WIN32)
#  include <windows.h>
#endif

#include "cmds/hook.hpp"
#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/registry.hpp"
#include "core/run_context.hpp"
#include "core/signals.hpp"
#include "core/trust.hpp"
#include "mtk/version.hpp"

namespace {

// Returns the absolute path of the currently-running mtk binary, or an
// empty path if it can't be resolved. Used by `mtk init <agent>` so the
// generated hook configs reference the actual binary by absolute path —
// crucial for build-only users who can't put mtk on the system PATH.
std::filesystem::path executable_path() noexcept {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = std::filesystem::canonical(buf, ec);
        if (!ec) return p;
    }
#elif defined(__linux__)
    std::error_code ec;
    auto p = std::filesystem::canonical("/proc/self/exe", ec);
    if (!ec) return p;
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::filesystem::path(buf);
#endif
    return {};
}

// Format a path for inclusion in a shell single-quoted string (the bash
// hook script). Escapes embedded single quotes via the classic
// `'\''` trick.
std::string shell_single_quote(const std::filesystem::path& p) {
    const auto s = p.string();
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

// Format a path for inclusion in a JSON string value (escapes \ and ").
std::string json_escape(const std::filesystem::path& p) {
    const auto s = p.string();
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            default:   out += c; break;
        }
    }
    return out;
}

void print_help() {
    std::cout
        << "mtk — Minimal Token Killer (" << mtk::kVersion << ")\n"
        << "\n"
        << "Usage:\n"
        << "  mtk <command> [args...]         Run command through registry dispatch\n"
        << "  mtk exec <command> [args...]    Same (alias)\n"
        << "  mtk explain <command> [args...] Show which filter would match (dry-run)\n"
        << "  mtk rewrite <command> [args...] Emit wrapped form if a filter matches\n"
        << "  mtk hook <agent>                Hook entry point (called by agent runtime)\n"
        << "  mtk init <agent>                Install agent hooks (claude / copilot / list)\n"
        << "  mtk trust [path]                Allow .mtk/filters/ at <path> (default: cwd)\n"
        << "  mtk untrust [path]              Remove <path> from allow-list\n"
        << "  mtk trusted                     List trusted paths\n"
        << "  mtk --version                   Print version\n"
        << "  mtk --help                      This help\n"
        << "\n"
        << "Examples:\n"
        << "  mtk git log -5\n"
        << "  mtk git status\n"
        << "  mtk grep \"fn\" src/\n"
        << "  mtk ls\n"
        << "  mtk make                        (uses TOML filter if installed)\n"
        << "\n"
        << "Environment:\n"
        << "  MTK_DEBUG=1                     Dispatch trace to stderr\n"
        << "  MTK_ALLOW_PROJECT_FILTERS=1     Bypass trust check (load any .mtk/filters/)\n";
}

const char* tier_label(mtk::core::Tier t) noexcept {
    switch (t) {
        case mtk::core::Tier::OrgToml:     return "org";
        case mtk::core::Tier::Builtin:     return "builtin";
        case mtk::core::Tier::UserToml:    return "user";
        case mtk::core::Tier::ProjectToml: return "project";
        case mtk::core::Tier::Fallback:    return "fallback";
    }
    return "unknown";
}

int run_explain(const std::vector<std::string>& argv) {
    auto reg = mtk::core::build_default_registry();

    if (!argv.empty()) {
        auto match = reg.find(argv);
        std::cout << "Would dispatch:\n";
        if (match.filter) {
            std::cout << "  filter:  " << match.filter->name() << '\n'
                      << "  source:  " << match.filter->source() << '\n';
        } else {
            std::cout << "  (no filter matched — registry misconfigured)\n";
        }
        std::cout << '\n';
    }

    std::cout << "Registry contents (priority order):\n";
    auto entries = reg.describe();
    for (const auto& e : entries) {
        std::cout << "  " << (e.shadowed ? "[SHADOWED] " : "           ")
                  << e.name
                  << "  (tier=" << tier_label(e.tier)
                  << ", source=" << e.source << ")\n";
    }
    return 0;
}

int run_trust(const std::vector<std::string>& argv) {
    std::filesystem::path target = argv.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(argv[0]);
    auto resolved = mtk::core::trust::canonicalise(target);
    if (resolved.empty()) return mtk::core::exit_codes::kUsage;
    if (mtk::core::trust::add(resolved)) {
        std::cout << "mtk trust: added " << resolved << '\n';
    } else {
        std::cout << "mtk trust: " << resolved << " already trusted (or write failed)\n";
    }
    return 0;
}

int run_untrust(const std::vector<std::string>& argv) {
    std::filesystem::path target = argv.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(argv[0]);
    auto resolved = mtk::core::trust::canonicalise(target);
    if (resolved.empty()) return mtk::core::exit_codes::kUsage;
    if (mtk::core::trust::remove(resolved)) {
        std::cout << "mtk untrust: removed " << resolved << '\n';
    } else {
        std::cout << "mtk untrust: " << resolved << " not in allow-list\n";
    }
    return 0;
}

int run_trusted() {
    auto list = mtk::core::trust::list();
    if (list.empty()) {
        std::cout << "(no trusted paths; "
                  << mtk::core::trust::allowed_projects_file() << " is empty or absent)\n";
        return 0;
    }
    std::cout << "Trusted paths (from "
              << mtk::core::trust::allowed_projects_file() << "):\n";
    for (const auto& p : list) std::cout << "  " << p << '\n';
    return 0;
}

// Per Phase 2 (2/2): companion to mtk init. Hook scripts call
// `mtk rewrite <cmd>` to decide whether to wrap the command in mtk. If a
// non-passthrough filter would match, emit `mtk <cmd>`. Otherwise echo
// the original — no point routing pure passthroughs through us.
int run_rewrite(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "mtk rewrite: no command given\n";
        return mtk::core::exit_codes::kUsage;
    }
    auto reg = mtk::core::build_default_registry();
    auto match = reg.find(argv);
    bool useful_match = match.filter &&
        match.filter->name() != std::string_view("_passthrough");

    if (useful_match) std::cout << "mtk ";
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) std::cout << ' ';
        std::cout << argv[i];
    }
    std::cout << '\n';
    return 0;
}

// Per Phase 2 (2/2): write agent-hook integration. Today: claude only.
// Others (copilot/gemini/codex/cursor) get TODO notes — each has a
// different hook protocol and we don't write configs blind.
int run_init_claude() {
    const char* home = std::getenv("HOME");
    if (!home) {
        std::cerr << "mtk init: $HOME not set\n";
        return mtk::core::exit_codes::kUsage;
    }
    std::filesystem::path claude_dir = std::filesystem::path(home) / ".claude";
    std::filesystem::path hooks_dir = claude_dir / "hooks";
    std::filesystem::path hook_script = hooks_dir / "mtk-rewrite.sh";

    std::error_code ec;
    std::filesystem::create_directories(hooks_dir, ec);
    if (ec) {
        std::cerr << "mtk init: failed to create " << hooks_dir << ": "
                  << ec.message() << '\n';
        return mtk::core::exit_codes::kNotFound;
    }

    auto bin = executable_path();
    if (bin.empty()) {
        std::cerr << "mtk init: could not resolve own binary path; "
                     "the generated hook script will use bare 'mtk' "
                     "(requires PATH).\n";
    }
    std::string mtk_invoc = bin.empty() ? std::string{"mtk"}
                                        : shell_single_quote(bin);

    std::ostringstream body;
    body << "#!/usr/bin/env bash\n"
         << "# mtk-rewrite.sh — Claude Code PreToolUse hook (Bash tool only).\n"
         << "# Generated by `mtk init claude`. Reads tool input JSON from\n"
         << "# stdin, rewrites Bash commands through mtk, writes modified\n"
         << "# JSON to stdout. Requires `jq`. mtk binary is referenced by\n"
         << "# absolute path so no PATH setup is needed.\n"
         << "set -euo pipefail\n"
         << "\n"
         << "MTK=" << mtk_invoc << "\n"
         << "\n"
         << "input=$(cat)\n"
         << "tool_name=$(printf '%s' \"$input\" | jq -r '.tool_name // empty')\n"
         << "if [[ \"$tool_name\" != \"Bash\" ]]; then\n"
         << "    printf '%s' \"$input\"\n"
         << "    exit 0\n"
         << "fi\n"
         << "\n"
         << "cmd=$(printf '%s' \"$input\" | jq -r '.tool_input.command // empty')\n"
         << "if [[ -z \"$cmd\" ]]; then\n"
         << "    printf '%s' \"$input\"\n"
         << "    exit 0\n"
         << "fi\n"
         << "\n"
         << "# mtk rewrite emits either \"mtk <cmd>\" (wrap) or \"<cmd>\" (passthrough).\n"
         << "rewritten=$(printf '%s' \"$cmd\" | xargs -0 -I{} \"$MTK\" rewrite {} \\\n"
         << "            2>/dev/null || printf '%s' \"$cmd\")\n"
         << "printf '%s' \"$input\" | jq --arg c \"$rewritten\" '.tool_input.command = $c'\n";

    std::ofstream f(hook_script);
    if (!f) {
        std::cerr << "mtk init: failed to write " << hook_script << '\n';
        return mtk::core::exit_codes::kNotFound;
    }
    f << body.str();
    f.close();
    std::filesystem::permissions(hook_script,
        std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write |
        std::filesystem::perms::owner_exec |
        std::filesystem::perms::group_read |
        std::filesystem::perms::group_exec |
        std::filesystem::perms::others_read |
        std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, ec);

    std::cout << "Wrote " << hook_script << " (chmod 755)\n"
              << "\n"
              << "Manual step — add to ~/.claude/settings.json (under \"hooks\"):\n"
              << "\n"
              << "  \"PreToolUse\": [\n"
              << "    {\n"
              << "      \"matcher\": \"Bash\",\n"
              << "      \"hooks\": [\n"
              << "        {\n"
              << "          \"type\": \"command\",\n"
              << "          \"command\": \"" << hook_script.string() << "\"\n"
              << "        }\n"
              << "      ]\n"
              << "    }\n"
              << "  ]\n"
              << "\n"
              << "Requires `jq`. mtk binary path is baked into the hook —\n"
              << "no PATH setup needed. Re-run `mtk init claude` if you\n"
              << "move the binary.\n";
    return 0;
}

int run_init_copilot() {
    std::filesystem::path github_dir = std::filesystem::current_path() / ".github";
    std::filesystem::path hooks_dir = github_dir / "hooks";
    std::filesystem::path hook_json = hooks_dir / "mtk-rewrite.json";
    std::filesystem::path instructions = github_dir / "copilot-instructions.md";

    std::error_code ec;
    std::filesystem::create_directories(hooks_dir, ec);
    if (ec) {
        std::cerr << "mtk init copilot: failed to create " << hooks_dir
                  << ": " << ec.message() << '\n';
        return mtk::core::exit_codes::kNotFound;
    }

    // Dual-format hook config: PreToolUse for VS Code Copilot Chat,
    // preToolUse for Copilot CLI. Both call `mtk hook copilot` which
    // detects the input shape and rewrites accordingly. Use the absolute
    // path of the current mtk binary so build-only users don't need PATH.
    auto bin = executable_path();
    if (bin.empty()) {
        std::cerr << "mtk init copilot: could not resolve own binary "
                     "path; the generated hook config will use bare "
                     "'mtk' (requires PATH).\n";
    }
    const std::string cmd = bin.empty()
        ? std::string{"mtk hook copilot"}
        : ('"' + json_escape(bin) + "\" hook copilot");

    std::ostringstream hook_json_stream;
    hook_json_stream
        << "{\n"
        << "  \"version\": 1,\n"
        << "  \"hooks\": {\n"
        << "    \"PreToolUse\": [\n"
        << "      {\n"
        << "        \"type\": \"command\",\n"
        << "        \"command\": " << (bin.empty() ? "\"mtk hook copilot\""
                                                   : ("\"" + json_escape(bin) + " hook copilot\""))
        << ",\n"
        << "        \"cwd\": \".\",\n"
        << "        \"timeout\": 5\n"
        << "      }\n"
        << "    ],\n"
        << "    \"preToolUse\": [\n"
        << "      {\n"
        << "        \"type\": \"command\",\n"
        << "        \"bash\": " << (bin.empty() ? "\"mtk hook copilot\""
                                                : ("\"" + json_escape(bin) + " hook copilot\""))
        << ",\n"
        << "        \"powershell\": " << (bin.empty() ? "\"mtk hook copilot\""
                                                      : ("\"" + json_escape(bin) + " hook copilot\""))
        << ",\n"
        << "        \"cwd\": \".\",\n"
        << "        \"timeoutSec\": 5\n"
        << "      }\n"
        << "    ]\n"
        << "  }\n"
        << "}\n";
    const std::string kHookJson = hook_json_stream.str();

    constexpr const char* kInstructionsBlock = R"(<!-- mtk-instructions v1 -->
# mtk — Token-Optimized CLI

**mtk** is a CLI proxy that filters and compresses shell command output,
reducing tokens spent on tool results.

## Rule

When a shell command would benefit from filtering, prefix it with `mtk`:

```bash
# Instead of:              Use:
git status                 mtk git status
git log -10                mtk git log -10
git diff                   mtk git diff
ls /some/path              mtk ls /some/path
grep "pattern" src/        mtk grep "pattern" src/
```

The hook at `.github/hooks/mtk-rewrite.json` does this automatically
for known filters. The instructions above bias the model toward emitting
already-prefixed commands so the hook has less to rewrite.

## Meta-commands

```bash
mtk explain <cmd>     # Show which filter would match (dry-run)
mtk trusted           # List trusted project-filter paths
```
<!-- /mtk-instructions -->
)";

    {
        std::ofstream f(hook_json);
        if (!f) {
            std::cerr << "mtk init copilot: failed to write " << hook_json << '\n';
            return mtk::core::exit_codes::kNotFound;
        }
        f << kHookJson;
    }
    (void)cmd;  // cmd built above for clarity; emission uses inline expressions

    // For the instructions file: upsert (don't clobber existing user content).
    // If the file exists and already has our marker block, replace it;
    // otherwise append.
    std::string existing;
    if (std::ifstream in(instructions); in) {
        std::ostringstream ss; ss << in.rdbuf();
        existing = ss.str();
    }
    auto start_marker = std::string("<!-- mtk-instructions");
    auto end_marker = std::string("<!-- /mtk-instructions -->");
    auto block_start = existing.find(start_marker);
    auto block_end = (block_start != std::string::npos)
        ? existing.find(end_marker, block_start) : std::string::npos;
    std::string updated;
    if (block_start != std::string::npos && block_end != std::string::npos) {
        // Replace existing block in place.
        updated = existing.substr(0, block_start);
        updated += kInstructionsBlock;
        updated += existing.substr(block_end + end_marker.size());
    } else {
        // Append (with a separating newline if the file is non-empty).
        updated = existing;
        if (!updated.empty() && updated.back() != '\n') updated += '\n';
        if (!updated.empty()) updated += '\n';
        updated += kInstructionsBlock;
    }
    {
        std::ofstream f(instructions);
        if (!f) {
            std::cerr << "mtk init copilot: failed to write " << instructions << '\n';
            return mtk::core::exit_codes::kNotFound;
        }
        f << updated;
    }

    std::cout << "Wrote " << hook_json << "\n"
              << "Wrote " << instructions
              << " (upserted mtk-instructions block)\n"
              << "\n"
              << "Restart your IDE or Copilot CLI session to activate.\n"
              << "\n"
              << "Notes:\n"
              << "  - Project-scoped — each repo needs its own `mtk init copilot`.\n"
              << "  - The mtk binary path is baked into the hook (no PATH needed).\n"
              << "    Re-run `mtk init copilot` if you move the binary.\n"
              << "  - Native subcommand (no jq required, unlike Claude).\n";
    return 0;
}

int run_init(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cout
            << "mtk init <agent>\n"
            << "\n"
            << "Agents:\n"
            << "  claude    — Claude Code (claude.ai/code) — IMPLEMENTED\n"
            << "  copilot   — GitHub Copilot (VS Code + CLI) — IMPLEMENTED\n"
            << "  gemini    — Gemini CLI — TODO\n"
            << "  codex     — OpenAI Codex CLI — TODO\n"
            << "  cursor    — Cursor IDE — TODO\n"
            << "\n"
            << "Examples:\n"
            << "  mtk init claude     # per-user hook in ~/.claude/hooks/\n"
            << "  mtk init copilot    # per-project hook in .github/hooks/\n";
        return 0;
    }
    const auto& agent = argv[0];
    if (agent == "claude") return run_init_claude();
    if (agent == "copilot") return run_init_copilot();
    std::cerr << "mtk init: agent '" << agent
              << "' not yet supported. Run `mtk init` for the list.\n";
    return mtk::core::exit_codes::kUsage;
}

int dispatch(const std::vector<std::string>& argv) {
    auto reg = mtk::core::build_default_registry();
    auto match = reg.find(argv);
    if (!match.filter) {
        std::cerr << "mtk: no filter matched (registry misconfigured)\n";
        return mtk::core::exit_codes::kNotFound;
    }

    if (const char* dbg = std::getenv("MTK_DEBUG");
        dbg && std::string_view(dbg) == "1") {
        std::cerr << "[mtk debug] dispatched to filter '" << match.filter->name()
                  << "' (source: " << match.filter->source() << ")\n";
    }

    mtk::core::RunContext ctx;
    auto outcome = match.filter->run(std::move(match.token), argv, ctx);
    return ctx.emit(std::move(outcome), match.filter->name());
}

}  // namespace

int main(int argc, char** argv) {
    mtk::core::signals::install();

    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_help();
        return 0;
    }
    if (args[0] == "--version") {
        std::cout << "mtk " << mtk::kVersion << '\n';
        return 0;
    }

    // Meta-commands resolved before registry dispatch.
    if (args[0] == "explain") {
        return run_explain(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "trust") {
        return run_trust(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "untrust") {
        return run_untrust(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "trusted") {
        return run_trusted();
    }
    if (args[0] == "rewrite") {
        return run_rewrite(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "init") {
        return run_init(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "hook") {
        if (args.size() < 2) {
            std::cerr << "mtk hook: missing agent (try `mtk hook copilot`)\n";
            return mtk::core::exit_codes::kUsage;
        }
        if (args[1] == "copilot") return mtk::cmds::hook::run_copilot();
        std::cerr << "mtk hook: agent '" << args[1] << "' not yet supported\n";
        return mtk::core::exit_codes::kUsage;
    }

    // Optional `exec` prefix for backward-compat: `mtk exec X` == `mtk X`.
    if (args[0] == "exec") {
        args.erase(args.begin());
        if (args.empty()) {
            std::cerr << "mtk exec: no command given\n";
            return mtk::core::exit_codes::kUsage;
        }
    }

    return dispatch(args);
}
