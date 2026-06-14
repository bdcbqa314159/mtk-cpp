#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/registry.hpp"
#include "core/run_context.hpp"
#include "core/signals.hpp"
#include "core/trust.hpp"
#include "mtk/version.hpp"

namespace {

void print_help() {
    std::cout
        << "mtk — Minimal Token Killer (" << mtk::kVersion << ")\n"
        << "\n"
        << "Usage:\n"
        << "  mtk <command> [args...]         Run command through registry dispatch\n"
        << "  mtk exec <command> [args...]    Same (alias)\n"
        << "  mtk explain <command> [args...] Show which filter would match (dry-run)\n"
        << "  mtk rewrite <command> [args...] Emit wrapped form if a filter matches\n"
        << "  mtk init <agent>                Install agent hooks (claude / list)\n"
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

    constexpr const char* kHookBody = R"(#!/usr/bin/env bash
# mtk-rewrite.sh — Claude Code PreToolUse hook (Bash tool only).
# Reads tool input JSON from stdin, rewrites Bash commands through mtk,
# writes modified JSON to stdout. Requires `jq` and `mtk` on PATH.
set -euo pipefail

input=$(cat)
tool_name=$(printf '%s' "$input" | jq -r '.tool_name // empty')
if [[ "$tool_name" != "Bash" ]]; then
    printf '%s' "$input"
    exit 0
fi

cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty')
if [[ -z "$cmd" ]]; then
    printf '%s' "$input"
    exit 0
fi

# mtk rewrite emits either "mtk <cmd>" (wrap) or "<cmd>" (passthrough).
rewritten=$(printf '%s' "$cmd" | xargs -0 -I{} mtk rewrite {} 2>/dev/null \
            || printf '%s' "$cmd")
printf '%s' "$input" | jq --arg c "$rewritten" '.tool_input.command = $c'
)";

    std::ofstream f(hook_script);
    if (!f) {
        std::cerr << "mtk init: failed to write " << hook_script << '\n';
        return mtk::core::exit_codes::kNotFound;
    }
    f << kHookBody;
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
              << "Requires `jq` and `mtk` on PATH.\n";
    return 0;
}

int run_init(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::cout
            << "mtk init <agent>\n"
            << "\n"
            << "Agents:\n"
            << "  claude    — Claude Code (claude.ai/code) — IMPLEMENTED\n"
            << "  copilot   — GitHub Copilot CLI — TODO (different hook protocol)\n"
            << "  gemini    — Gemini CLI — TODO\n"
            << "  codex     — OpenAI Codex CLI — TODO\n"
            << "  cursor    — Cursor IDE — TODO\n"
            << "\n"
            << "Example: mtk init claude\n";
        return 0;
    }
    const auto& agent = argv[0];
    if (agent == "claude") return run_init_claude();
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
