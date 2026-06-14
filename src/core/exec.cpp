#include "core/exec.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#include "core/limits.hpp"
#include "core/signals.hpp"

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

// Staged termination: SIGTERM, wait kGraceMs, then SIGKILL.
reproc::stop_actions staged_stop() {
    reproc::stop_actions sa{};
    sa.first.action = reproc::stop::terminate;
    sa.first.timeout = reproc::milliseconds(mtk::core::limits::signal_::kGraceMs);
    sa.second.action = reproc::stop::kill;
    sa.second.timeout = reproc::milliseconds(0);
    return sa;
}

int passthrough(const std::vector<std::string>& argv) {
    if (argv.empty()) return 127;
    if (!resolvable_command(argv[0])) {
        std::fprintf(stderr, "mtk %s: command not found in PATH\n", argv[0].c_str());
        return 127;
    }

    reproc::process proc;
    reproc::options opts;
    opts.redirect.parent = true;
    opts.stop = staged_stop();

    if (auto ec = proc.start(argv, opts)) {
        std::fprintf(stderr, "mtk %s: failed to spawn: %s\n", argv[0].c_str(),
                     ec.message().c_str());
        return 127;
    }

    // Poll loop so we can react to signals (the child shares our process group
    // so it gets the signal too; we still want to wait for it to exit and
    // propagate the exit code rather than dying first).
    while (true) {
        if (int sig = mtk::core::signals::pending(); sig != 0) {
            mtk::core::signals::clear();
            // proc::~process / .stop() will run the staged termination.
            (void)proc.terminate();
            auto [status, _ec] = proc.wait(
                reproc::milliseconds(mtk::core::limits::signal_::kGraceMs));
            (void)status;
            return 128 + sig;
        }
        auto [status, ec] = proc.wait(reproc::milliseconds(100));
        if (!ec || ec.value() != static_cast<int>(std::errc::timed_out)) {
            return status;
        }
    }
}

// Bounded sink: appends to `target` while a shared byte-counter stays under
// `cap`. Stops the drain (non-zero error_code) when the cap is reached. The
// counter is shared between stdout and stderr sinks so the cap is the total
// captured bytes per A6.
class BoundedSink {
public:
    BoundedSink(std::string& target, std::size_t& total, std::size_t cap,
                bool& truncated) noexcept
        : target_(target), total_(total), cap_(cap), truncated_(truncated) {}

    std::error_code operator()(reproc::stream, const uint8_t* buf, std::size_t size) {
        if (total_ >= cap_) {
            truncated_ = true;
            return std::make_error_code(std::errc::file_too_large);
        }
        std::size_t room = cap_ - total_;
        std::size_t to_write = (size < room) ? size : room;
        target_.append(reinterpret_cast<const char*>(buf), to_write);
        total_ += to_write;
        if (to_write < size) {
            truncated_ = true;
            return std::make_error_code(std::errc::file_too_large);
        }
        return {};
    }

private:
    std::string& target_;
    std::size_t& total_;
    std::size_t cap_;
    bool& truncated_;
};

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
    opts.deadline = reproc::milliseconds(mtk::core::limits::capture::kTimeoutMs);
    opts.stop = staged_stop();

    if (!env_extra.empty()) {
        opts.env.behavior = reproc::env::extend;
        opts.env.extra = env_extra;
    }

    if (auto ec = proc.start(argv, opts)) {
        return SpawnFailed{ec.message()};
    }

    Ran ran;
    std::size_t total_bytes = 0;
    bool truncated = false;
    BoundedSink out_sink(ran.stdout_data, total_bytes,
                         mtk::core::limits::capture::kMaxBytes, truncated);
    BoundedSink err_sink(ran.stderr_data, total_bytes,
                         mtk::core::limits::capture::kMaxBytes, truncated);

    (void)reproc::drain(proc, out_sink, err_sink);
    ran.truncated = truncated;

    // If we cut the drain short because of the byte cap, the child is still
    // running. Terminate it via the staged stop_actions configured in opts.
    if (truncated) {
        (void)proc.terminate();
    }

    int pending_sig = mtk::core::signals::pending();
    if (pending_sig != 0) {
        mtk::core::signals::clear();
        (void)proc.terminate();
        ran.killed_by_signal = pending_sig;
    }

    auto [status, wait_ec] = proc.wait(reproc::infinite);
    ran.exit_code = status;

    // reproc's deadline expired before the child exited cleanly.
    if (wait_ec && wait_ec.value() == static_cast<int>(std::errc::timed_out)) {
        ran.timed_out = true;
        ran.exit_code = 124;  // canonical timeout exit
    }

    if (ran.killed_by_signal != 0) {
        ran.exit_code = 128 + ran.killed_by_signal;
    }

    // Fallback for the case PATH resolution can't catch: fork succeeded but
    // execvp inside the child failed silently (e.g., race conditions with
    // PATH changes between resolve and exec, exec-bit missing, ENOEXEC).
    // The child immediately exits 127 with no output in this case.
    if (ran.exit_code == 127 && ran.stdout_data.empty() && ran.stderr_data.empty() &&
        !ran.truncated && !ran.timed_out && ran.killed_by_signal == 0) {
        return SpawnFailed{"command not found in PATH: " + argv[0]};
    }
    return ran;
}

}  // namespace mtk::core::exec
