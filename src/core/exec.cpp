#include "core/exec.hpp"

#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

namespace mtk::core::exec {

CapturedOutput capture(const std::vector<std::string>& argv,
                       const EnvExtra& env_extra) {
    CapturedOutput out;
    if (argv.empty()) {
        out.spawn_error = "empty argv";
        return out;
    }

    reproc::process proc;
    reproc::options opts;
    opts.redirect.in.type = reproc::redirect::type::discard;
    opts.redirect.out.type = reproc::redirect::type::pipe;
    opts.redirect.err.type = reproc::redirect::type::pipe;

    if (!env_extra.empty()) {
        opts.env.behavior = reproc::env::extend;
        opts.env.extra = env_extra;
    }

    if (auto ec = proc.start(argv, opts)) {
        out.spawn_error = ec.message();
        out.exit_code = 127;
        return out;
    }
    out.spawned = true;

    reproc::sink::string out_sink(out.stdout_data);
    reproc::sink::string err_sink(out.stderr_data);
    (void)reproc::drain(proc, out_sink, err_sink);

    auto [status, _wait_ec] = proc.wait(reproc::infinite);
    out.exit_code = status;
    return out;
}

int passthrough(const std::vector<std::string>& argv) {
    if (argv.empty()) return 127;

    reproc::process proc;
    reproc::options opts;
    opts.redirect.parent = true;

    if (auto ec = proc.start(argv, opts)) {
        return 127;
    }
    auto [status, _ec] = proc.wait(reproc::infinite);
    return status;
}

}  // namespace mtk::core::exec
