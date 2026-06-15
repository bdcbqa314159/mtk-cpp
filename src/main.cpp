#include <chrono>
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
#include "core/audit.hpp"
#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/filter_cache.hpp"
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
        << "  mtk reload                      Invalidate + rebuild TOML-filter cache\n"
        << "  mtk stats                       Per-filter savings dashboard\n"
        << "  mtk tail [N]                    Last N audit events (default 10)\n"
        << "  mtk why <event-id>              Re-spool raw output of an event\n"
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
        << "  MTK_ALLOW_PROJECT_FILTERS=1     Bypass trust check (load any .mtk/filters/)\n"
        << "  MTK_AUDIT_PAYLOAD=1             Capture full output to ~/.local/state/mtk/payloads/\n";
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

int run_reload() {
    // Per perf critic P2: invalidate the cache. Next mtk invocation will
    // re-parse all TOMLs and rebuild the binary cache on the way out.
    bool removed = mtk::core::filter_cache::invalidate();
    if (removed) {
        std::cout << "mtk reload: cache invalidated ("
                  << mtk::core::filter_cache::cache_file()
                  << " removed). Next invocation re-parses all TOML filters.\n";
    } else {
        std::cout << "mtk reload: no cache to invalidate ("
                  << mtk::core::filter_cache::cache_file()
                  << " does not exist). Next invocation will build one.\n";
    }
    // Eagerly rebuild now so the user sees parse errors immediately
    // rather than on next dispatch.
    (void)mtk::core::build_default_registry();
    std::cout << "mtk reload: cache rebuilt.\n";
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

// --- Phase 3: audit-reading meta-commands ---

std::string fmt_bytes(std::size_t n) {
    char buf[32];
    if (n >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / (1024.0 * 1024.0));
    } else if (n >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(n) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%zuB", n);
    }
    return buf;
}

std::string fmt_argv(const std::vector<std::string>& argv, std::size_t max_len = 60) {
    std::string s;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) s += ' ';
        s += argv[i];
        if (s.size() > max_len) {
            s.resize(max_len);
            s += "…";
            break;
        }
    }
    return s;
}

int run_tail(const std::vector<std::string>& argv) {
    std::size_t n = 10;
    if (!argv.empty()) {
        try { n = std::stoul(argv[0]); } catch (...) {
            std::cerr << "mtk tail: usage: mtk tail [N]\n";
            return mtk::core::exit_codes::kUsage;
        }
    }
    auto events = mtk::core::audit::tail(n);
    if (events.empty()) {
        std::cout << "(no audit events — "
                  << mtk::core::audit::log_file() << " is empty or absent)\n";
        return 0;
    }
    for (const auto& e : events) {
        long savings_pct = 0;
        if (e.bytes_in > 0) {
            savings_pct = 100 - static_cast<long>(
                e.bytes_out * 100 / e.bytes_in);
        }
        std::cout << e.ts << "  " << e.event_id
                  << "  " << e.filter_name
                  << "  exit=" << e.exit_code
                  << "  " << fmt_bytes(e.bytes_in) << "→" << fmt_bytes(e.bytes_out)
                  << " (" << savings_pct << "%)"
                  << "  " << e.elapsed_ms << "ms"
                  << "  " << fmt_argv(e.argv)
                  << '\n';
    }
    return 0;
}

int run_stats() {
    auto events = mtk::core::audit::read_all();
    if (events.empty()) {
        std::cout << "(no audit events — "
                  << mtk::core::audit::log_file() << " is empty or absent)\n";
        return 0;
    }

    struct Agg {
        std::size_t count = 0;
        std::size_t bytes_in = 0;
        std::size_t bytes_out = 0;
        std::size_t errors = 0;
        long elapsed_ms_total = 0;
    };
    std::unordered_map<std::string, Agg> by_filter;
    Agg overall;
    for (const auto& e : events) {
        auto& f = by_filter[e.filter_name];
        f.count++;
        f.bytes_in += e.bytes_in;
        f.bytes_out += e.bytes_out;
        if (e.exit_code != 0) f.errors++;
        f.elapsed_ms_total += e.elapsed_ms;

        overall.count++;
        overall.bytes_in += e.bytes_in;
        overall.bytes_out += e.bytes_out;
        if (e.exit_code != 0) overall.errors++;
        overall.elapsed_ms_total += e.elapsed_ms;
    }

    auto pct = [](std::size_t in, std::size_t out) -> long {
        return in > 0 ? 100 - static_cast<long>(out * 100 / in) : 0;
    };

    std::cout << "mtk stats — " << events.size() << " events in "
              << mtk::core::audit::log_file() << "\n\n";
    std::cout << "Overall:\n"
              << "  bytes_in:   " << fmt_bytes(overall.bytes_in) << "\n"
              << "  bytes_out:  " << fmt_bytes(overall.bytes_out)
              << "  (savings " << pct(overall.bytes_in, overall.bytes_out) << "%)\n"
              << "  errors:     " << overall.errors << " / " << overall.count
              << " (" << (overall.count > 0 ? overall.errors * 100 / overall.count : 0)
              << "%)\n"
              << "  avg time:   "
              << (overall.count > 0 ? overall.elapsed_ms_total / static_cast<long>(overall.count) : 0)
              << "ms\n\n";

    std::vector<std::pair<std::string, Agg>> sorted(by_filter.begin(), by_filter.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

    std::cout << "By filter (sorted by count):\n";
    std::cout << "  count  filter         bytes_in→out      sav%  err  avg_ms\n";
    for (const auto& [name, f] : sorted) {
        std::printf("  %5zu  %-12s  %6s→%-6s  %4ld%% %4zu  %5ld\n",
                    f.count,
                    name.c_str(),
                    fmt_bytes(f.bytes_in).c_str(),
                    fmt_bytes(f.bytes_out).c_str(),
                    pct(f.bytes_in, f.bytes_out),
                    f.errors,
                    f.count > 0 ? f.elapsed_ms_total / static_cast<long>(f.count) : 0);
    }
    return 0;
}

int run_why(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cerr << "mtk why: usage: mtk why <event-id>\n";
        return mtk::core::exit_codes::kUsage;
    }
    auto path = mtk::core::audit::payload_path(argv[0]);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::cout << "[mtk why: no payload captured for " << argv[0] << "]\n"
                  << "  Payload capture is opt-in via MTK_AUDIT_PAYLOAD=1.\n"
                  << "  Re-run the command with that env set to capture future\n"
                  << "  events; today's outputs aren't recoverable.\n"
                  << "  Expected at: " << path << "\n";
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "mtk why: failed to read " << path << '\n';
        return mtk::core::exit_codes::kNotFound;
    }
    std::cout << f.rdbuf();
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

    const std::string filter_name(match.filter->name());
    const std::string filter_source(match.filter->source());

    if (const char* dbg = std::getenv("MTK_DEBUG");
        dbg && std::string_view(dbg) == "1") {
        std::cerr << "[mtk debug] dispatched to filter '" << filter_name
                  << "' (source: " << filter_source << ")\n";
    }

    mtk::core::RunContext ctx;
    auto start = std::chrono::steady_clock::now();
    auto outcome = match.filter->run(std::move(match.token), argv, ctx);

    // Capture audit fields BEFORE emit consumes the outcome.
    std::size_t bytes_out = 0;
    bool timed_out = false;
    int killed_by_signal = 0;
    bool bytes_in_capped = false;
    if (const auto* ran = ctx.as_ran(outcome)) {
        bytes_out = ran->stdout_data.size() + ran->stderr_data.size();
        timed_out = ran->timed_out;
        killed_by_signal = ran->killed_by_signal;
        bytes_in_capped = ran->truncated;
    }

    // Optional payload capture under MTK_AUDIT_PAYLOAD=1.
    auto event_id = mtk::core::audit::make_event_id();
    if (const auto* ran = ctx.as_ran(outcome)) {
        (void)mtk::core::audit::capture_payload(event_id, ran->stdout_data);
    }

    int exit_code = ctx.emit(std::move(outcome), filter_name);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    ctx.audit(mtk::core::RunContext::AuditEvent{
        event_id,
        filter_name,
        filter_source,
        argv,
        exit_code,
        /*bytes_in*/ 0,  // 0 = take from cumulative_bytes_in()
        bytes_out,
        static_cast<long>(elapsed),
        bytes_in_capped,
        timed_out,
        killed_by_signal,
    });

    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    // Per perf critic P11: untie iostreams from C stdio. We do mixed
    // `std::cout` (control-plane diagnostics) and `std::fwrite(stdout)`
    // (hot-path dispatch output) — without this each iostream operation
    // flushes the C buffer to keep ordering. Setting this at startup is
    // the canonical way to ditch the sync overhead.
    std::ios_base::sync_with_stdio(false);

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
    if (args[0] == "reload") {
        return run_reload();
    }
    if (args[0] == "rewrite") {
        return run_rewrite(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "init") {
        return run_init(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "stats") {
        return run_stats();
    }
    if (args[0] == "tail") {
        return run_tail(std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (args[0] == "why") {
        return run_why(std::vector<std::string>(args.begin() + 1, args.end()));
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
