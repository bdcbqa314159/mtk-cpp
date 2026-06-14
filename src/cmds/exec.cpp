#include "cmds/exec.hpp"

#include <iostream>

#include "core/default_registry.hpp"
#include "core/exit_codes.hpp"
#include "core/registry.hpp"
#include "core/run_context.hpp"

namespace mtk::cmds::exec {

// Phase 1.5 (1/2): registry-driven dispatch for the catch-all path.
// Loads the default registry (TOML filters + Passthrough), finds the
// matching filter (always at least Passthrough), runs it, emits.
//
// Phase 1.5 (2/2) will move git/ls/grep into the same registry, and
// main.cpp's CLI11 subcommand dispatch will collapse into a single
// loop calling this function with the full argv.
int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "mtk exec: no command given\n";
        return mtk::core::exit_codes::kUsage;
    }

    auto reg = mtk::core::build_default_registry();
    auto match = reg.find(args);
    if (!match.filter) {
        // Unreachable — Passthrough always matches. Defensive return.
        std::cerr << "mtk exec: no filter matched (registry misconfigured)\n";
        return mtk::core::exit_codes::kNotFound;
    }

    mtk::core::RunContext ctx;
    auto outcome = match.filter->run(std::move(match.token), args, ctx);
    return ctx.emit(std::move(outcome), match.filter->name());
}

}  // namespace mtk::cmds::exec
