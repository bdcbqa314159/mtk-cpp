#include "core/exec.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

namespace mtk::core::exec {

namespace {

// Returns true if argv[0] is plausibly resolvable: either it contains a
// path separator (let exec try) or it's findable on PATH. False only when
// PATH was checked exhaustively and the binary isn't there. Surfaces
// "command not found" before we fork — see A3's PATH pre-resolution.
bool resolvable_command(std::string_view cmd) noexcept {
    if (cmd.empty()) return false;
    if (cmd.find('/') != std::string_view::npos) return true;
    const char* path_env = std::getenv("PATH");
    if (!path_env) return true;

    std::string path_str(path_env);
    std::size_t start = 0;
    while (start <= path_str.size()) {
        std::size_t end = path_str.find(':', start);
        if (end == std::string::npos) end = path_str.size();
        if (end > start) {
            std::filesystem::path candidate(path_str.substr(start, end - start));
            candidate /= std::string(cmd);
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) return true;
        }
        if (end == path_str.size()) break;
        start = end + 1;
    }
    return false;
}

}  // namespace

int passthrough(const std::vector<std::string>& argv) {
    if (argv.empty()) return 127;
    if (!resolvable_command(argv[0])) {
        std::fprintf(stderr, "mtk %s: command not found in PATH\n", argv[0].c_str());
        return 127;
    }

    reproc::process proc;
    reproc::options opts;
    opts.redirect.parent = true;

    if (auto ec = proc.start(argv, opts)) {
        std::fprintf(stderr, "mtk %s: failed to spawn: %s\n", argv[0].c_str(),
                     ec.message().c_str());
        return 127;
    }
    auto [status, _ec] = proc.wait(reproc::infinite);
    return status;
}

ExecOutcome capture_outcome(const std::vector<std::string>& argv,
                            const EnvExtra& env_extra) {
    if (argv.empty()) {
        return SpawnFailed{"empty argv"};
    }
    if (!resolvable_command(argv[0])) {
        return SpawnFailed{"command not found in PATH: " + argv[0]};
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
        return SpawnFailed{ec.message()};
    }

    Ran ran;
    reproc::sink::string out_sink(ran.stdout_data);
    reproc::sink::string err_sink(ran.stderr_data);
    (void)reproc::drain(proc, out_sink, err_sink);

    auto [status, _wait_ec] = proc.wait(reproc::infinite);
    ran.exit_code = status;

    // Fallback for the case PATH resolution can't catch: fork succeeded but
    // execvp inside the child failed silently (e.g., race conditions with
    // PATH changes between resolve and exec, exec-bit missing, ENOEXEC).
    // The child immediately exits 127 with no output in this case.
    if (ran.exit_code == 127 && ran.stdout_data.empty() && ran.stderr_data.empty()) {
        return SpawnFailed{"command not found in PATH: " + argv[0]};
    }
    return ran;
}

}  // namespace mtk::core::exec
