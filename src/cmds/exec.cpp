#include "cmds/exec.hpp"

#include <iostream>

#include "core/config.hpp"
#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/run_context.hpp"
#include "core/toml_filter.hpp"

namespace mtk::cmds::exec {

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "mtk exec: no command given\n";
        return mtk::core::exit_codes::kUsage;
    }

    auto filters = mtk::core::config::load_all_filters();
    auto match = mtk::core::config::find_filter_for(args[0], filters);

    mtk::core::RunContext ctx;
    if (!match) {
        return ctx.passthrough(args);
    }

    auto outcome = ctx.capture(args);
    const auto* ran = ctx.as_ran(outcome);
    if (!ran) {
        return ctx.report_spawn_failure(outcome, args[0]);
    }

    const std::string blob = match->filter_stderr
        ? (ran->stdout_data + ran->stderr_data)
        : ran->stdout_data;

    std::string filtered;
    try {
        filtered = mtk::core::toml_filter::apply(*match, blob);
    } catch (const std::exception& e) {
        std::cerr << "mtk exec: filter warning [" << match->name << "]: " << e.what() << '\n';
        filtered = blob;
    }

    std::cout << filtered;
    if (!filtered.empty() && filtered.back() != '\n') std::cout << '\n';

    if (!match->filter_stderr && !ran->stderr_data.empty()) {
        std::cerr << ran->stderr_data;
    }

    ctx.tee_on_failure(outcome, args[0]);
    return ran->exit_code;
}

}  // namespace mtk::cmds::exec
