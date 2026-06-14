#include "cmds/exec.hpp"

#include <iostream>

#include "core/config.hpp"
#include "core/exec.hpp"
#include "core/exit_codes.hpp"
#include "core/tee.hpp"
#include "core/toml_filter.hpp"

namespace mtk::cmds::exec {

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "mtk exec: no command given\n";
        return 2;
    }

    auto filters = mtk::core::config::load_all_filters();
    auto match = mtk::core::config::find_filter_for(args[0], filters);

    if (!match) {
        return mtk::core::exec::passthrough(args);
    }

    auto captured = mtk::core::exec::capture(args);
    if (!captured.spawned) {
        return mtk::core::exit_codes::report_spawn_failure(args[0], captured.spawn_error);
    }

    const std::string& blob = match->filter_stderr
        ? (captured.stdout_data + captured.stderr_data)
        : captured.stdout_data;

    std::string filtered;
    try {
        filtered = mtk::core::toml_filter::apply(*match, blob);
    } catch (const std::exception& e) {
        std::cerr << "mtk: filter warning [" << match->name << "]: " << e.what() << '\n';
        filtered = blob;
    }

    std::cout << filtered;
    if (!filtered.empty() && filtered.back() != '\n') std::cout << '\n';

    if (!match->filter_stderr && !captured.stderr_data.empty()) {
        std::cerr << captured.stderr_data;
    }
    if (captured.exit_code != 0) {
        if (auto hint = mtk::core::tee::tee_and_hint(blob, args[0], captured.exit_code)) {
            std::cerr << *hint << '\n';
        }
    }
    return captured.exit_code;
}

}  // namespace mtk::cmds::exec
