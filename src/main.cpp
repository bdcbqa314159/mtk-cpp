#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <filesystem>

#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/registry.hpp"
#include "core/run_context.hpp"
#include "core/signals.hpp"
#include "core/trust.hpp"

namespace {

void print_help() {
    std::cout
        << "mtk — Minimal Token Killer (v0.1.0)\n"
        << "\n"
        << "Usage:\n"
        << "  mtk <command> [args...]         Run command through registry dispatch\n"
        << "  mtk exec <command> [args...]    Same (alias)\n"
        << "  mtk explain <command> [args...] Show which filter would match (dry-run)\n"
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
        std::cout << "mtk 0.1.0\n";
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
