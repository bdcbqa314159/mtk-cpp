#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cmds/hook.hpp"
#include "cmds/init.hpp"
#include "cmds/meta.hpp"
#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/registry.hpp"
#include "core/run_context.hpp"
#include "core/signals.hpp"
#include "mtk/version.hpp"

namespace {

void print_help() {
    std::cout
        << "mtk -- Minimal Token Killer (" << mtk::kVersion << ")\n"
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

    // Per audit: post-run choreography (timing, outcome introspection,
    // payload capture, emit, audit) lives in RunContext::run_and_audit
    // — the dispatcher no longer reaches into the outcome to assemble
    // the AuditEvent itself.
    mtk::core::RunContext ctx;
    return ctx.run_and_audit(*match.filter, std::move(match.token), argv,
                             filter_name, filter_source);
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

    auto rest = [&]() {
        return std::vector<std::string>(args.begin() + 1, args.end());
    };

    // Meta-commands resolved before registry dispatch.
    namespace meta = mtk::cmds::meta;
    if (args[0] == "explain") return meta::run_explain(rest());
    if (args[0] == "trust")   return meta::run_trust(rest());
    if (args[0] == "untrust") return meta::run_untrust(rest());
    if (args[0] == "trusted") return meta::run_trusted();
    if (args[0] == "reload")  return meta::run_reload();
    if (args[0] == "rewrite") return meta::run_rewrite(rest());
    if (args[0] == "init")    return mtk::cmds::init::run_init(rest());
    if (args[0] == "stats")   return meta::run_stats();
    if (args[0] == "tail")    return meta::run_tail(rest());
    if (args[0] == "why")     return meta::run_why(rest());

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
